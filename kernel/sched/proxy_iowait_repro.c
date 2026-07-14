// SPDX-License-Identifier: GPL-2.0
/*
 * proxy_iowait_repro.c
 *
 * Regression reproducer for the proxy-execution per-cpu nr_iowait imbalance
 * fixed by:
 *
 *   d346306db401 ("sched: proxy-exec: fix nr_iowait imbalance in
 *                   do_activate_blocked_waiter()")
 *
 * ---------------------------------------------------------------------------
 * Porting notes (built-in variant)
 * ---------------------------------------------------------------------------
 * Originally a loadable module that ran the reproducer once at module_init()
 * time and resolved the un-exported nr_iowait_cpu() via a kprobe. Built into
 * kernel/sched/ instead, so:
 *   - nr_iowait_cpu() is called directly (declared in <linux/sched/stat.h>);
 *     no kprobe / CONFIG_KPROBES needed.
 *   - the reproducer is *not* run at boot. It is triggered on demand through a
 *     sysfs node:
 *
 *         echo 1 > /sys/kernel/proxy_iowait_repro/run
 *
 *     which auto-picks the first two online CPUs as donor/owner and runs one
 *     reproduction pass. Results go to dmesg. A run mutex serialises triggers.
 *
 * Requirements:
 *   - CONFIG_SCHED_PROXY_EXEC=y  and proxy-exec enabled at runtime (default on)
 *   - >= 2 online CPUs
 *
 * ---------------------------------------------------------------------------
 * The bug
 * ---------------------------------------------------------------------------
 * nr_iowait is a *per-rq* (per-cpu) counter. With proxy execution, a task that
 * has p->in_iowait == 1 and blocks on a struct mutex whose owner is *sleeping*
 * (!owner->on_rq) does NOT go through the normal io_schedule() dequeue. Instead:
 *
 *   find_proxy_task()                            [kernel/sched/core.c]
 *     owner !on_rq  ->  proxy_enqueue_on_owner()
 *                         block_task() -> __block_task()
 *                           if (p->in_iowait) atomic_inc(&rq->nr_iowait)
 *                                                        ^^ DONOR's cpu (rq)
 *
 * Later the owner wakes up on some *other* cpu and reverses the accounting:
 *
 *   try_to_wake_up(owner)                        [tail]
 *     activate_blocked_waiters(cpu_rq(task_cpu(owner)), owner, ...)
 *       do_activate_blocked_waiter(target_rq, p, ...)
 *
 *         buggy order:                     fixed order (this tree):
 *           proxy_set_task_cpu(p, tgt);      if (p->in_iowait)
 *           ...                                atomic_dec(&task_rq(p)->nr_iowait);
 *           atomic_dec(&task_rq(p)->..);     proxy_set_task_cpu(p, tgt);
 *                    ^^ task_rq(p)==target            ^^ task_rq(p)==donor cpu
 *                       (OWNER's cpu) - WRONG             (correct)
 *
 * So on a buggy kernel the inc lands on the donor cpu and the dec lands on the
 * owner cpu: the donor cpu leaks +1 forever (its idle time is then miscounted
 * as iowait by account_idle_time(), inflating WALT util / cpufreq), while the
 * owner cpu is driven to -1. The global nr_iowait() sum stays ~balanced, which
 * is exactly why this hid for so long.
 *
 * ---------------------------------------------------------------------------
 * The reproducer
 * ---------------------------------------------------------------------------
 *   owner thread (pinned to owner_cpu):
 *       mutex_lock(M); tell main; sleep holding M (=> owner goes !on_rq)
 *   donor thread (pinned to donor_cpu):
 *       mutex_lock_io(M)  => in_iowait=1, then blocks on M
 *
 *   main:
 *       base   = nr_iowait_cpu(donor_cpu), nr_iowait_cpu(owner_cpu)
 *       start owner, wait until it is asleep off-rq holding M
 *       start donor, wait until proxy_enqueue_on_owner() has parked it
 *         (donor->sleeping_owner == owner)  =>  nr_iowait[donor_cpu] == base+1
 *       wake owner  => do_activate_blocked_waiter() runs the dec
 *       measure again:
 *         fixed kernel : donor delta 0,  owner delta 0     -> BALANCED
 *         buggy kernel : donor delta +1, owner delta -1    -> IMBALANCE
 */

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/stat.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kstrtox.h>

static int donor_cpu = -1;
static int owner_cpu = -1;

static struct mutex	test_mutex;
static struct completion owner_has_lock;	/* owner -> main   */
static struct completion donor_go;		/* main  -> donor  */
static struct completion owner_should_wake;	/* main  -> owner  */
static struct completion owner_done;		/* owner -> main   */
static struct completion donor_done;		/* donor -> main   */

static struct task_struct *owner_task;
static struct task_struct *donor_task;

/* Serialise sysfs-triggered runs: the state above is shared/global. */
static DEFINE_MUTEX(repro_lock);

static int owner_fn(void *unused)
{
	mutex_lock(&test_mutex);		/* acquire M */
	complete(&owner_has_lock);		/* tell main we hold M */

	/*
	 * Sleep while still holding M. This makes the owner go !on_rq, which
	 * is the precondition for find_proxy_task() to take the
	 * proxy_enqueue_on_owner() branch for the donor below.
	 */
	wait_for_completion(&owner_should_wake);

	mutex_unlock(&test_mutex);		/* hand M off to the donor */
	complete(&owner_done);
	return 0;
}

static int donor_fn(void *unused)
{
	wait_for_completion(&donor_go);

	/* io_schedule_prepare() sets in_iowait=1, then we block on M. */
	mutex_lock_io(&test_mutex);
	mutex_unlock(&test_mutex);

	complete(&donor_done);
	return 0;
}

static unsigned long long sum_iowait_online(void)
{
	unsigned long long sum = 0;
	int c;

	for_each_online_cpu(c)
		sum += nr_iowait_cpu(c);
	return sum;
}

/* Auto-pick the first two online CPUs as donor/owner (they always differ). */
static int pick_cpus(void)
{
	int first = -1, c;

	donor_cpu = -1;
	owner_cpu = -1;

	for_each_online_cpu(c) {
		if (first < 0) {
			first = c;
			continue;
		}
		donor_cpu = first;
		owner_cpu = c;
		return 0;
	}

	pr_err("pe_iowait: need >= 2 online cpus to reproduce a cross-cpu imbalance\n");
	return -ENODEV;
}

static int run_repro(void)
{
	int base_d, base_o, mid_d, mid_o, fin_d, fin_o;
	unsigned long long g_base, g_fin;
	bool pe_parked = false;
	int dd, od;
	int i, ret = 0;

	mutex_init(&test_mutex);
	init_completion(&owner_has_lock);
	init_completion(&donor_go);
	init_completion(&owner_should_wake);
	init_completion(&owner_done);
	init_completion(&donor_done);

	base_d = nr_iowait_cpu(donor_cpu);
	base_o = nr_iowait_cpu(owner_cpu);
	g_base = sum_iowait_online();
	pr_info("pe_iowait: START donor_cpu=%d owner_cpu=%d | baseline nr_iowait[d]=%d nr_iowait[o]=%d global=%llu\n",
		donor_cpu, owner_cpu, base_d, base_o, g_base);

	/* ---- owner takes M and goes to sleep holding it ---- */
	owner_task = kthread_create(owner_fn, NULL, "pe_iow_owner");
	if (IS_ERR(owner_task)) {
		ret = PTR_ERR(owner_task);
		owner_task = NULL;
		return ret;
	}
	get_task_struct(owner_task);
	kthread_bind(owner_task, owner_cpu);
	wake_up_process(owner_task);

	if (!wait_for_completion_timeout(&owner_has_lock, msecs_to_jiffies(5000))) {
		pr_err("pe_iowait: owner never acquired the mutex\n");
		ret = -ETIMEDOUT;
		goto release_owner;
	}
	/* Wait until the owner is actually off the runqueue (sleeping). */
	for (i = 0; i < 5000 && READ_ONCE(owner_task->on_rq); i++)
		msleep(1);
	if (READ_ONCE(owner_task->on_rq)) {
		pr_err("pe_iowait: owner did not go !on_rq; cannot exercise proxy_enqueue_on_owner()\n");
		ret = -ETIMEDOUT;
		goto release_owner;
	}

	/* ---- donor: in_iowait + block on M; PE should park it on the owner ---- */
	donor_task = kthread_create(donor_fn, NULL, "pe_iow_donor");
	if (IS_ERR(donor_task)) {
		ret = PTR_ERR(donor_task);
		donor_task = NULL;
		goto release_owner;
	}
	get_task_struct(donor_task);
	kthread_bind(donor_task, donor_cpu);
	wake_up_process(donor_task);
	complete(&donor_go);

	/*
	 * Wait for the nr_iowait inc to land on donor_cpu. block_task() (the
	 * inc) runs inside proxy_enqueue_on_owner(), which also sets
	 * donor->sleeping_owner == owner just before it -- we sample that as a
	 * definitive signal that the *proxy* parking path (not a plain
	 * io_schedule block) is what incremented the counter. We break on the
	 * measured counter, not on sleeping_owner, to avoid the tiny window
	 * where sleeping_owner is set but block_task() has not run yet.
	 */
	for (i = 0; i < 5000; i++) {
#if IS_ENABLED(CONFIG_SCHED_PROXY_EXEC)
		if (READ_ONCE(donor_task->sleeping_owner) == owner_task)
			pe_parked = true;
#endif
		if (nr_iowait_cpu(donor_cpu) == base_d + 1)
			break;
		msleep(1);
	}

	mid_d = nr_iowait_cpu(donor_cpu);
	mid_o = nr_iowait_cpu(owner_cpu);
	pr_info("pe_iowait: donor blocked | nr_iowait[d]=%d (%+d) nr_iowait[o]=%d (%+d) parked_on_owner=%d\n",
		mid_d, mid_d - base_d, mid_o, mid_o - base_o, pe_parked);

	if (mid_d != base_d + 1)
		pr_warn("pe_iowait: donor cpu nr_iowait never reached +1 -- donor may not have blocked on donor_cpu; result will be inconclusive\n");
	else if (!pe_parked)
		pr_warn("pe_iowait: donor blocked but proxy_enqueue_on_owner() did NOT run (proxy-exec off at runtime?); the buggy dec path is not exercised, expect BALANCED regardless of the fix\n");

	/* ---- wake the owner on owner_cpu: runs do_activate_blocked_waiter() ---- */
	complete(&owner_should_wake);

	if (!wait_for_completion_timeout(&owner_done, msecs_to_jiffies(5000)))
		pr_warn("pe_iowait: owner thread did not finish in time\n");
	if (!wait_for_completion_timeout(&donor_done, msecs_to_jiffies(5000)))
		pr_warn("pe_iowait: donor thread did not finish in time\n");

	msleep(50);				/* let the dec settle */
	fin_d = nr_iowait_cpu(donor_cpu);
	fin_o = nr_iowait_cpu(owner_cpu);
	g_fin = sum_iowait_online();
	dd = fin_d - base_d;
	od = fin_o - base_o;

	pr_info("pe_iowait: FINAL   | nr_iowait[d]=%d (%+d) nr_iowait[o]=%d (%+d) global=%llu (base %llu)\n",
		fin_d, dd, fin_o, od, g_fin, g_base);

	if (dd == 0 && od == 0) {
		pr_info("pe_iowait: RESULT = BALANCED  => fix present (bug NOT reproduced)\n");
	} else if (dd == 1 && od == -1) {
		pr_err("pe_iowait: RESULT = IMBALANCE REPRODUCED => BUG PRESENT\n");
		pr_err("pe_iowait:   donor_cpu %d leaked nr_iowait +1; owner_cpu %d driven to -1\n",
		       donor_cpu, owner_cpu);
		pr_err("pe_iowait:   NOTE: donor_cpu nr_iowait is now permanently elevated -- its\n");
		pr_err("pe_iowait:   idle time will be miscounted as iowait until reboot.\n");
	} else {
		pr_warn("pe_iowait: RESULT = INCONCLUSIVE (donor %+d, owner %+d) -- retry on quieter CPUs\n",
			dd, od);
	}

	put_task_struct(donor_task);
	put_task_struct(owner_task);
	return ret;

release_owner:
	/* Make sure nothing is left blocked on our completions. */
	complete(&owner_should_wake);
	wait_for_completion_timeout(&owner_done, msecs_to_jiffies(5000));
	if (donor_task) {
		wait_for_completion_timeout(&donor_done, msecs_to_jiffies(5000));
		put_task_struct(donor_task);
	}
	put_task_struct(owner_task);
	return ret;
}

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sysfs_emit(buf,
		"write 1 to run the proxy-exec nr_iowait imbalance reproducer\n"
		"(auto-picks the first two online CPUs as donor/owner; results go to dmesg)\n");
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	bool go;
	int ret;

	if (kstrtobool(buf, &go))
		return -EINVAL;
	if (!go)
		return count;

	if (!mutex_trylock(&repro_lock)) {
		pr_info("pe_iowait: a run is already in progress\n");
		return -EBUSY;
	}

	if (!IS_ENABLED(CONFIG_SCHED_PROXY_EXEC))
		pr_warn("pe_iowait: CONFIG_SCHED_PROXY_EXEC=n -- the buggy code path does not exist; expect BALANCED\n");

	ret = pick_cpus();
	if (!ret)
		run_repro();

	mutex_unlock(&repro_lock);
	return count;
}

static struct kobj_attribute run_attr =
	__ATTR(run, 0664, run_show, run_store);

static struct attribute *proxy_iowait_repro_attrs[] = {
	&run_attr.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group proxy_iowait_repro_attr_group = {
	.attrs = proxy_iowait_repro_attrs,
};

static struct kobject *proxy_iowait_repro_kobj;

static int __init proxy_iowait_repro_init(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_SCHED_PROXY_EXEC))
		pr_warn("pe_iowait: CONFIG_SCHED_PROXY_EXEC=n at build time -- the buggy code path does not exist; expect BALANCED\n");

	proxy_iowait_repro_kobj = kobject_create_and_add("proxy_iowait_repro",
							 kernel_kobj);
	if (!proxy_iowait_repro_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(proxy_iowait_repro_kobj,
				 &proxy_iowait_repro_attr_group);
	if (ret)
		kobject_put(proxy_iowait_repro_kobj);
	return ret;
}
late_initcall(proxy_iowait_repro_init);
