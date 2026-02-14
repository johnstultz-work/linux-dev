.. SPDX-License-Identifier: GPL-2.0

===============
Proxy Execution
===============

Proxy Execution is a new scheduling feature for the Linux kernel.
This doc covers why it is needed, and how it works.

Priority inversion issues
=========================

On many systems, there are the concepts of foreground and background
tasks. Ideally you want the foreground tasks the user is interacting with
to be prioritized, and you want to make sure the background tasks don't
interfere or interrupt foreground tasks. Additionally, you want to make
sure you limit the amount of CPU and battery those background tasks can
consume.

Unfortunately when most mechanisms are used to constrain or restrict background
tasks, allowing foreground tasks to be prioritized, you see average
latencies improve as wanted, but there is a strange side-effect: the outlier
latencies grow dramatically\! This might seem counterintuitive, but those versed
in operating system design recognize it as a very well understood issue:
Priority Inversion.

Linux uses `rt_mutex` structs which provide priority inheritance as its
solution to this problem. However, `rt_mutex` structs have limitations. They
really only help with RT tasks, which have a linear strict priority order.
Further, RT priorities are only recommended for trusted system apps only as
these priorities can be unsafe for third-party apps, making them unsuitable
for prioritizing foreground applications over background ones.

Additionally, (apart from using `CONFIG_PREEMPT_RT`) `rt_mutex` structs are
not always used in the kernel, and normal in-kernel mutexes (and
other common kernel locking primitives) can be a common source of priority
inversion.

In Linux, there is no inheritance for `rt_mutex` structs for fair-class
(`SCHED_NORMAL/BATCH`) waiters. The assumption is that with the fair class,
all tasks get approximately equal runtime (ignoring the nice weighting), so
priority inversion cannot occur indefinitely.

While not indefinite, priority inversion can still occur for long enough
to be problematic, causing interactivity latencies (jank) and other
delays.

Thus there is a need for a solution that:

*   Provides inheritable priority across complex scheduling
    orderings (nested cgroups, vruntime, deadlines) where there might not be a
    linear priority order, so that it can work across all scheduler classes
    (deadline, realtime, fair, scx, and idle)
*   Supports priority inheritance for locking primitives other than
    `rt_mutex` structs

Generalized inheritance with Proxy Execution
============================================

Proxy Execution provides this generalized form of priority inheritance that
supports in-kernel mutexes and `rw_semaphore` structs.

To illustrate this, consider a small benchmark that uses a foreground and
background task that do the same file system operations in a directory, as well
as some CPU spinner tasks to generate load. Measuring the latencies of the
foreground task completing the operations shows a normal distribution of
latencies, sometimes as high as low double digit milliseconds, when the
background and spinner tasks are unconstrained.

When the test constrains the background and spinner tasks using cpu-cgroup
`cpu_shares` (or `weight`), the average latency improves, but the
worst-case outliers balloon to hundreds of milisconds  due to priority
inversion (where the background task holds a lock needed by the foreground
task, and has to wait for it to be scheduled to proceed).

But with Proxy Execution, the test can constrain background tasks and improve
the average latency of foreground tasks without the negative impact of priority
inversion, because it provides priority inheritance.

Proxy Execution overview
========================

In Proxy Execution, when a task blocks on a kernel mutex (or `rw_semaphore`),
the waiter donates its time to the lock owner, so the owner can run and
release the lock.

The first step is to leave mutex blocked tasks on the runqueue. Note
that the task is marked as blocked (using `task->blocked_on`), so it
doesn't run if selected by the scheduler.

The second is what is called the split-context. Instead of just using `rq->curr`
to track the running task, the scheduler uses two separate pointers (`rq->curr`
and `rq->donor`). The `rq->donor` is the task almost all of the scheduling
accounting is done against, effectively charging it for the time. The
`rq->curr` is the task that is actually running. (This change is already
upstream, and if `CONFIG_SCHED_PROXY_EXECUTION` is disabled, the `rq->curr` and
`rq->donor` are a union that use the same pointer so they are always the same.)

With Proxy Execution scheduler is treated as an opaque box, allowing it to pick
whatever is the most important task to run out of the set of runnable tasks as
well as mutex blocked tasks. If it picks a non-blocked task, both the
`rq->curr` and `rq->donor` are set to the selected task from `pick_next_task()`,
and things carry on as they always have.

It's really only when the scheduler selects a mutex-blocked task (which is
evaluated from the `task->blocked_on` structure) that much is done differently.
In that case the `rq->donor` is set to the task the scheduler selected using
`pick_next_task()`, and then `find_proxy_task()` is called, which looks up the
mutex owner (and if that mutex owner is blocked on another mutex. It traverses
the chain to find that mutex's owner) and returns the runnable owner task. If no
task is returned (more on why this could happen below), the
`__schedule()` logic is restarted to pick another task.

The `rq->curr` pointer is then set to the task returned by `find_proxy_task()`,
and the scheduler switches to running it.

However, Proxy Execution must handle several complications:

*  In `find_proxy_task()`, if the lock is released while searching for the
   mutex owner, the function returns `NULL` and picks again.
*  If the mutex owning task is on a different CPU's runqueue,
   `find_proxy_task()` dequeues the selected donor task from this runqueue,
   and migrates it to the remote CPU's runqueue. This allows the important
   task to be selected on that runqueue to run the lock owner. This might
   seem counterintuitive (as opposed to migrating the owner to the donor's
   CPU), but the lock owner might be SMP-affined, such that it cannot run on
   another CPU. The donor task doesn't run on the remote CPU; it only
   donates its time. Also, consider that there are potentially many donors
   on different CPUs waiting on the lock, so migrating them to the single
   owner is easier than pulling the owner in different directions. When the
   lock is released and the donor is woken up, Proxy Execution
   return-migrates the donor. This mechanism is behaviorally similar to the
   mutex-blocked task being removed from a runqueue and then added back when
   the lock is released and it is woken up.
*  If the mutex owning task is sleeping (off the runqueue), dequeue the
   selected donor task, add its `task->blocked_node` to the
   `task->blocked_head` list on the owner's task-struct, and return `NULL` to
   pick again. When the owner is woken up and returned to a runqueue, add
   any waiting dequeued tasks on the owner's `blocked_head` list to the same
   runqueue.
*  There are a few transient edge cases (usually where a race has
   occurred) where the function returns NULL and forces the scheduler to
   pick again (see the `find_proxy_task()` logic for details).

Also, within `find_proxy_task()` there are situations where it might want to
migrate or dequeue a task, but that task is at that moment running (`on_cpu`).
In that case, it returns `rq->idle` to run as `rq->curr`, but avoids clearing
`need_resched`, so that `__schedule()` will context switch to idle briefly,
getting the current task off the CPU, then re-enter `__schedule()` so that
task can be migrated.

Another change is that when the lock is released and the waiting donor
task is woken up, logic detects it was a blocked donor task and dequeues
it from the runqueue so the wakeup logic can work properly. Otherwise,
the `try_to_wake_up()` logic might shortcut assuming a task on the
runqueue was already woken, potentially allowing a donor task to run on a
CPU it was not affined for.

Outside of the core scheduler changes, Proxy Execution changes little else.
The mutex lock logic remains mostly the same as without Proxy
Execution; the `wait_list` and its order are unmodified. The only
exception with mutexes is that an explicit lock handoff to the donor is
attempted when the owner releases the lock while being proxied
(effectively rewarding the donor for its donation).

More information
================

See related conference videos:

* [OSPM23](https://www.youtube.com/watch?v=QEWqRhVS3lI)
* [OSPM24](https://www.youtube.com/watch?v=wI36v6vCfkg)
* [LPC24](https://www.youtube.com/watch?v=FgB26cdJDZk)
* [OSPM25](https://www.youtube.com/watch?v=xcV1NtWENbs)
* [LPC25](https://www.youtube.com/watch?v=Ab65z2klt9w)

Relevant upstream lkml patch submissions:

* [Preparatory changes for Proxy Execution](https://lore.kernel.org/lkml/20240829225212.6042-1-jstultz@google.com/) (merged upstream)
* [Single RunQueue Proxy Execution](https://lore.kernel.org/lkml/20250712033407.2383110-1-jstultz@google.com/) (merged upstream)
* [Donor Migration for Proxy Execution](https://lore.kernel.org/lkml/20251124223111.3616950-1-jstultz@google.com/)


