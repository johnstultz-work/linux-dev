// SPDX-License-Identifier: GPL-2.0
/*
 * test_wq_race - Reproducer for the workqueue lockup race triggered by
 * a reschedule IPI preempting a worker between schedule() invoking
 * wq_worker_sleeping() and wq_worker_sleeping() decrementing
 * pool->nr_running.
 *
 * Harness model:
 *   - two per-CPU bound workqueues with default attrs so they attach to the
 *     same normal worker_pool on target_cpu:
 *       * wq-race-test
 *       * wq-race-gp
 *   - many race_work_fn items contend on a single mutex
 *   - the mutex owner queues gp_work_fn onto wq-race-gp on target_cpu and
 *     then waits for a completion while still holding the mutex
 *   - a higher-priority helper kthread is pinned to target_cpu and is
 *     repeatedly woken from another CPU so the scheduler sets
 *     TIF_NEED_RESCHED remotely and sends the reschedule IPI for us
 *
 * If the race fires, pool->nr_running gets stuck > 0 even though all busy
 * workers are sleeping. The queued gp_work_fn never gets dispatched, the
 * mutex owner never drops the lock, and the rest of the workers pile up
 * behind the mutex on the same pool.
 *
 * Without test_wq_race.enable=1 the reproducer does nothing at boot.
 */

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Set to 1 to start the reproducer at init (default off)");

static int target_cpu = 1;
module_param(target_cpu, int, 0444);
MODULE_PARM_DESC(target_cpu, "CPU to bind contending workers to");

static int ipi_cpu = -1;
module_param(ipi_cpu, int, 0444);
MODULE_PARM_DESC(ipi_cpu, "CPU to run the wake thread on (-1 = auto-pick)");

static int monitor_cpu = -1;
module_param(monitor_cpu, int, 0444);
MODULE_PARM_DESC(monitor_cpu, "CPU to run the monitor thread on (-1 = auto-pick)");

static int nr_workers = 6;
module_param(nr_workers, int, 0444);
MODULE_PARM_DESC(nr_workers, "Number of contending work items");

static int ipi_burst = 1;
module_param(ipi_burst, int, 0644);
MODULE_PARM_DESC(ipi_burst, "Wakeups sent per burst");

static int ipi_delay_us = 1000;
module_param(ipi_delay_us, int, 0644);
MODULE_PARM_DESC(ipi_delay_us, "usleep between wakeup bursts");

static int iterations;
module_param(iterations, int, 0644);
MODULE_PARM_DESC(iterations, "Lock+wait iterations per worker before exit (0 = infinite)");

static int hold_lock_us;
module_param(hold_lock_us, int, 0644);
MODULE_PARM_DESC(hold_lock_us, "Optional randomized udelay max while holding the mutex before queueing gp work");

static int gp_hold_us;
module_param(gp_hold_us, int, 0644);
MODULE_PARM_DESC(gp_hold_us, "Optional randomized udelay max in gp_work_fn before completing");
module_param_named(reader_hold_us, gp_hold_us, int, 0644);
MODULE_PARM_DESC(reader_hold_us, "Compatibility alias for gp_hold_us");

static int compat_reader_gap_us;
module_param_named(reader_gap_us, compat_reader_gap_us, int, 0644);
MODULE_PARM_DESC(reader_gap_us, "Compatibility no-op parameter");

static int monitor_interval_ms = 1000;
module_param(monitor_interval_ms, int, 0644);
MODULE_PARM_DESC(monitor_interval_ms, "Monitor thread reporting period");

static int stall_threshold_ms = 1000;
module_param(stall_threshold_ms, int, 0644);
MODULE_PARM_DESC(stall_threshold_ms, "No-progress threshold before stopping the wake thread");

static int dump_interval_ms = 5000;
module_param(dump_interval_ms, int, 0644);
MODULE_PARM_DESC(dump_interval_ms, "Periodic workqueue dump interval (0 = disabled)");

static struct workqueue_struct *test_wq;
static struct workqueue_struct *gp_wq;
static DEFINE_MUTEX(test_lock);

struct race_work {
	struct work_struct work;
	struct work_struct gp_work;
	struct completion gp_done;
	int id;
	atomic_t iters_done;
};

static struct race_work *workers;
static atomic_t total_done;
static atomic_t in_flight;
static atomic_t lock_holders;
static atomic_t gp_queued;
static atomic_t gp_completed;

static struct task_struct *waker_thread;
static struct task_struct *preempt_thread;
static struct task_struct *monitor_thread;
static bool stop_requested;

static bool wq_race_stopping(void)
{
	return READ_ONCE(stop_requested);
}

static void stop_wake_path(void)
{
	WRITE_ONCE(stop_requested, true);
	if (waker_thread) {
		kthread_stop(waker_thread);
		waker_thread = NULL;
	}
	if (preempt_thread)
		wake_up_process(preempt_thread);
}

static void wq_race_random_udelay(int max_us)
{
	u32 delay_us;

	if (max_us <= 0)
		return;

	delay_us = get_random_u32_below((u32)max_us + 1);
	if (delay_us)
		udelay(delay_us);
}

static void gp_work_fn(struct work_struct *work)
{
	struct race_work *rw = container_of(work, struct race_work, gp_work);

	set_worker_desc("gp-run");

	wq_race_random_udelay(gp_hold_us);

	atomic_inc(&gp_completed);
	complete(&rw->gp_done);
}

static void race_work_fn(struct work_struct *work)
{
	struct race_work *rw = container_of(work, struct race_work, work);
	int done;

	wq_race_random_udelay(hold_lock_us);
	atomic_inc(&in_flight);

	/*
	 * Many workers all queue here. Mutex contention forces frequent
	 * schedule() calls, which is exactly what feeds the
	 * wq_worker_sleeping() preemption window.
	 */
	set_worker_desc("mutex-wait");
	mutex_lock(&test_lock);
	set_worker_desc("lock-held");
	atomic_inc(&lock_holders);

	/*
	 * Queue a same-pool dependency and wait for it while still holding the
	 * mutex. If the pool stops dispatching because nr_running stays stale,
	 * this completion never arrives.
	 */
	set_worker_desc("queue-gp");
	reinit_completion(&rw->gp_done);
	atomic_inc(&gp_queued);
	if (WARN_ON_ONCE(!queue_work_on(target_cpu, gp_wq, &rw->gp_work)))
		complete(&rw->gp_done);

	set_worker_desc("gp-wait");
	wait_for_completion(&rw->gp_done);

	set_worker_desc("unlock");
	atomic_dec(&lock_holders);
	mutex_unlock(&test_lock);

	done = atomic_inc_return(&rw->iters_done);
	atomic_inc(&total_done);
	atomic_dec(&in_flight);

	if (!wq_race_stopping() && (!iterations || done < iterations))
		queue_work_on(target_cpu, test_wq, &rw->work);
}

static int preempt_target_fn(void *unused)
{
	pr_info("wq-race: preempt helper on CPU %d\n", smp_processor_id());

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (wq_race_stopping())
			break;
		schedule();
	}

	__set_current_state(TASK_RUNNING);
	pr_info("wq-race: preempt helper exiting\n");
	return 0;
}

static int wake_storm_fn(void *unused)
{
	pr_info("wq-race: wake thread on CPU %d targeting CPU %d\n",
		smp_processor_id(), target_cpu);

	while (!kthread_should_stop() && !wq_race_stopping()) {
		int i;

		for (i = 0; i < ipi_burst && !kthread_should_stop(); i++) {
			if (preempt_thread)
				wake_up_process(preempt_thread);
		}

		if (ipi_delay_us > 0)
			usleep_range(ipi_delay_us, ipi_delay_us + 10);
		else
			cond_resched();
	}

	pr_info("wq-race: wake thread exiting\n");
	return 0;
}

static int monitor_fn(void *unused)
{
	unsigned long last_total = 0;
	unsigned long last_jiffies = jiffies;
	unsigned long last_dump_jiffies = jiffies;
	unsigned long stall_ms = 0;

	while (!kthread_should_stop() && !wq_race_stopping()) {
		unsigned long total, dt_ms;

		msleep_interruptible(monitor_interval_ms);

		total = atomic_read(&total_done);
		dt_ms = jiffies_to_msecs(jiffies - last_jiffies);

		if (dump_interval_ms > 0 &&
		    time_after_eq(jiffies,
				  last_dump_jiffies + msecs_to_jiffies(dump_interval_ms))) {
			pr_info("wq-race: periodic workqueue dump\n");
			show_all_workqueues();
			last_dump_jiffies = jiffies;
		}

		if (total == last_total) {
			int inflight = atomic_read(&in_flight);
			unsigned long prev_stall_ms = stall_ms;

			stall_ms += dt_ms;

			if (!inflight) {
				stop_wake_path();
				pr_info("wq-race: test complete, no race triggered (total_done=%lu gp_queued=%d gp_completed=%d)\n",
					total, atomic_read(&gp_queued),
					atomic_read(&gp_completed));
				break;
			}

			if (stall_ms >= stall_threshold_ms) {
				pr_warn("wq-race: STALL %lums total_done=%lu in_flight=%d lock_holders=%d gp_queued=%d gp_completed=%d\n",
					stall_ms, total, inflight,
					atomic_read(&lock_holders),
					atomic_read(&gp_queued),
					atomic_read(&gp_completed));

				if (prev_stall_ms < stall_threshold_ms) {
					stop_wake_path();
					pr_info("wq-race: wake path stopped, dumping workqueue state\n");
					show_all_workqueues();
					break;
				}
			}
		} else {
			stall_ms = 0;
			pr_info("wq-race: progress total_done=%lu (+%lu in %lums) in_flight=%d lock_holders=%d gp_queued=%d gp_completed=%d\n",
				total, total - last_total, dt_ms,
				atomic_read(&in_flight),
				atomic_read(&lock_holders),
				atomic_read(&gp_queued),
				atomic_read(&gp_completed));
		}

		last_total = total;
		last_jiffies = jiffies;
	}

	return 0;
}

static int pick_ipi_cpu(void)
{
	int cpu;

	if (ipi_cpu >= 0 && cpu_online(ipi_cpu) && ipi_cpu != target_cpu)
		return ipi_cpu;

	for_each_online_cpu(cpu) {
		if (cpu != target_cpu)
			return cpu;
	}

	return -1;
}

static int pick_monitor_cpu(int picked_ipi_cpu)
{
	int cpu;

	if (monitor_cpu >= 0 && cpu_online(monitor_cpu) &&
	    monitor_cpu != target_cpu && monitor_cpu != picked_ipi_cpu)
		return monitor_cpu;

	for_each_online_cpu(cpu) {
		if (cpu != target_cpu && cpu != picked_ipi_cpu)
			return cpu;
	}

	if (picked_ipi_cpu >= 0 && picked_ipi_cpu != target_cpu)
		return picked_ipi_cpu;

	return -1;
}

static int __init wq_race_init(void)
{
	int i, picked_ipi_cpu, picked_monitor_cpu;

	if (!enable) {
		pr_info("wq-race: dormant (set test_wq_race.enable=1 on cmdline to start)\n");
		return 0;
	}

	if (!cpu_online(target_cpu)) {
		pr_err("wq-race: target_cpu %d is not online\n", target_cpu);
		return -EINVAL;
	}

	picked_ipi_cpu = pick_ipi_cpu();
	if (picked_ipi_cpu < 0) {
		pr_err("wq-race: need at least 2 online CPUs\n");
		return -EINVAL;
	}

	picked_monitor_cpu = pick_monitor_cpu(picked_ipi_cpu);

	test_wq = alloc_workqueue("wq-race-test", 0, 0);
	if (!test_wq)
		return -ENOMEM;

	gp_wq = alloc_workqueue("wq-race-gp", 0, 0);
	if (!gp_wq) {
		destroy_workqueue(test_wq);
		test_wq = NULL;
		return -ENOMEM;
	}

	workers = kcalloc(nr_workers, sizeof(*workers), GFP_KERNEL);
	if (!workers) {
		destroy_workqueue(gp_wq);
		gp_wq = NULL;
		destroy_workqueue(test_wq);
		test_wq = NULL;
		return -ENOMEM;
	}

	atomic_set(&total_done, 0);
	atomic_set(&in_flight, 0);
	atomic_set(&lock_holders, 0);
	atomic_set(&gp_queued, 0);
	atomic_set(&gp_completed, 0);
	WRITE_ONCE(stop_requested, false);

	for (i = 0; i < nr_workers; i++) {
		workers[i].id = i;
		atomic_set(&workers[i].iters_done, 0);
		init_completion(&workers[i].gp_done);
		INIT_WORK(&workers[i].work, race_work_fn);
		INIT_WORK(&workers[i].gp_work, gp_work_fn);
	}

	preempt_thread = kthread_create(preempt_target_fn, NULL,
					"wq-race-preempt");
	if (IS_ERR(preempt_thread)) {
		int err = PTR_ERR(preempt_thread);

		preempt_thread = NULL;
		kfree(workers);
		workers = NULL;
		destroy_workqueue(gp_wq);
		gp_wq = NULL;
		destroy_workqueue(test_wq);
		test_wq = NULL;
		return err;
	}
	kthread_bind(preempt_thread, target_cpu);
	sched_set_fifo_low(preempt_thread);

	waker_thread = kthread_create(wake_storm_fn, NULL, "wq-race-waker");
	if (IS_ERR(waker_thread)) {
		int err = PTR_ERR(waker_thread);

		kthread_stop(preempt_thread);
		preempt_thread = NULL;
		waker_thread = NULL;
		kfree(workers);
		workers = NULL;
		destroy_workqueue(gp_wq);
		gp_wq = NULL;
		destroy_workqueue(test_wq);
		test_wq = NULL;
		return err;
	}
	kthread_bind(waker_thread, picked_ipi_cpu);
	sched_set_normal(waker_thread, 19);

	monitor_thread = kthread_create(monitor_fn, NULL, "wq-race-mon");
	if (IS_ERR(monitor_thread)) {
		int err = PTR_ERR(monitor_thread);

		kthread_stop(waker_thread);
		waker_thread = NULL;
		kthread_stop(preempt_thread);
		preempt_thread = NULL;
		monitor_thread = NULL;
		kfree(workers);
		workers = NULL;
		destroy_workqueue(gp_wq);
		gp_wq = NULL;
		destroy_workqueue(test_wq);
		test_wq = NULL;
		return err;
	}
	if (picked_monitor_cpu >= 0)
		kthread_bind(monitor_thread, picked_monitor_cpu);
	sched_set_fifo_low(monitor_thread);

	pr_info("wq-race: starting target_cpu=%d ipi_cpu=%d monitor_cpu=%d nr_workers=%d ipi_burst=%d ipi_delay_us=%d hold_lock_us=%d gp_hold_us=%d monitor_interval_ms=%d stall_threshold_ms=%d dump_interval_ms=%d iterations=%d\n",
		target_cpu, picked_ipi_cpu, picked_monitor_cpu, nr_workers,
		ipi_burst, ipi_delay_us, hold_lock_us, gp_hold_us,
		monitor_interval_ms, stall_threshold_ms, dump_interval_ms,
		iterations);
	pr_info("wq-race: bindings test_wq=%d gp_wq=%d preempt=%d waker=%d monitor=%d\n",
		target_cpu, target_cpu, target_cpu, picked_ipi_cpu,
		picked_monitor_cpu);

	wake_up_process(monitor_thread);
	wake_up_process(preempt_thread);
	wake_up_process(waker_thread);

	for (i = 0; i < nr_workers; i++)
		queue_work_on(target_cpu, test_wq, &workers[i].work);

	return 0;
}

late_initcall(wq_race_init);

MODULE_AUTHOR("Vineeth Pillai (Google) <vineeth@bitbyteword.org>");
MODULE_DESCRIPTION("Reproducer for wq_worker_sleeping/IPI race causing pool->nr_running stall");
MODULE_LICENSE("GPL");
