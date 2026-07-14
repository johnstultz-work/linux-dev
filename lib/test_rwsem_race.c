// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Test");
MODULE_DESCRIPTION("Reproduction test for rwsem mark_wake blocked_on race via sysfs");

static atomic_t run_count = ATOMIC_INIT(0);

static DECLARE_RWSEM(sem_a);
static DECLARE_RWSEM(sem_b);

static int reader_fn(void *arg)
{
	down_read(&sem_a);

	down_read(&sem_b);
	up_read(&sem_b);

	up_read(&sem_a);
	return 0;
}

static int run_test(void)
{
	//struct task_struct *thread_locker;
	struct task_struct *thread_reader;

	pr_info("rwsem_race_test: Starting test run #%d (sem_a=%p, sem_b=%p)\n",
		atomic_read(&run_count) + 1, &sem_a, &sem_b);

	down_write(&sem_a);
	down_write(&sem_b);

	thread_reader = kthread_run(reader_fn, NULL, "test_reader");
	if (IS_ERR(thread_reader))
		goto out_unlock;

	/* Let reader_fn get blocked in down_read(&sem_a) */
	msleep(100);

	wake_up_process(thread_reader);
	up_write(&sem_a);

	msleep(100);

	up_write(&sem_b);

	pr_info("rwsem_race_test: Test run #%d sequence completed\n", atomic_read(&run_count) + 1);
	return 0;

out_unlock:
	up_write(&sem_a);
	up_write(&sem_b);
	return -ENOMEM;
}

static ssize_t run_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "Run count: %d\nTo run test: echo 1 > /sys/kernel/test_rwsem_race/run\n",
			  atomic_read(&run_count));
}

static ssize_t run_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int err;

	err = run_test();
	if (err)
		return err;

	atomic_inc(&run_count);
	return count;
}

static struct kobj_attribute run_attribute = __ATTR_RW(run);

static struct attribute *test_rwsem_attrs[] = {
	&run_attribute.attr,
	NULL,
};

static struct attribute_group test_rwsem_attr_group = {
	.attrs = test_rwsem_attrs,
};

static struct kobject *test_rwsem_kobj;

static int __init test_rwsem_race_init(void)
{
	int ret;

	test_rwsem_kobj = kobject_create_and_add("test_rwsem_race", kernel_kobj);
	if (!test_rwsem_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(test_rwsem_kobj, &test_rwsem_attr_group);
	if (ret) {
		kobject_put(test_rwsem_kobj);
		return ret;
	}

	pr_info("rwsem_race_test: Registered /sys/kernel/test_rwsem_race/run\n");
	return 0;
}

static void __exit test_rwsem_race_exit(void)
{
	if (test_rwsem_kobj) {
		sysfs_remove_group(test_rwsem_kobj, &test_rwsem_attr_group);
		kobject_put(test_rwsem_kobj);
	}
	pr_info("rwsem_race_test: Unregistered sysfs interface\n");
}

late_initcall(test_rwsem_race_init);
module_exit(test_rwsem_race_exit);
