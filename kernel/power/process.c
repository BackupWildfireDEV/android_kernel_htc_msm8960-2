

#undef DEBUG

#include <linux/interrupt.h>
#include <linux/oom.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/kmod.h>
#include <linux/wakelock.h>
#include "power.h"
#include <mach/msm_watchdog.h>

#define TIMEOUT	(20 * HZ)

static int try_to_freeze_tasks(bool user_only)
{
	struct task_struct *g, *p;
	unsigned long end_time;
	unsigned int todo;
	bool wq_busy = false;
	struct timeval start, end;
	u64 elapsed_msecs64;
	unsigned int elapsed_msecs;
	bool wakeup = false;
	int sleep_usecs = USEC_PER_MSEC;

	do_gettimeofday(&start);

	end_time = jiffies + TIMEOUT;

	if (!user_only)
		freeze_workqueues_begin();

	while (true) {
		todo = 0;
		read_lock(&tasklist_lock);
		do_each_thread(g, p) {
			if (p == current || !freeze_task(p))
				continue;

			if (!task_is_stopped_or_traced(p) &&
			    !freezer_should_skip(p))
				todo++;
		} while_each_thread(g, p);
		read_unlock(&tasklist_lock);

		if (!user_only) {
			wq_busy = freeze_workqueues_busy();
			todo += wq_busy;
		}

		if (todo && has_wake_lock(WAKE_LOCK_SUSPEND)) {
			wakeup = 1;
			break;
		}
		if (!todo || time_after(jiffies, end_time))
			break;

		if (pm_wakeup_pending()) {
			wakeup = true;
			break;
		}

		/*
		 * We need to retry, but first give the freezing tasks some
		 * time to enter the refrigerator.  Start with an initial
		 * 1 ms sleep followed by exponential backoff until 8 ms.
		 */
		usleep_range(sleep_usecs / 2, sleep_usecs);
		if (sleep_usecs < 8 * USEC_PER_MSEC)
			sleep_usecs *= 2;
	}

	do_gettimeofday(&end);
	elapsed_msecs64 = timeval_to_ns(&end) - timeval_to_ns(&start);
	do_div(elapsed_msecs64, NSEC_PER_MSEC);
	elapsed_msecs = elapsed_msecs64;

	if (todo) {
		printk("\n");
		printk(KERN_ERR "Freezing of tasks %s after %d.%03d seconds "
		       "(%d tasks refusing to freeze, wq_busy=%d):\n",
		       wakeup ? "aborted" : "failed",
		       elapsed_msecs / 1000, elapsed_msecs % 1000,
		       todo - wq_busy, wq_busy);

		if (!wakeup) {
#ifdef CONFIG_MSM_WATCHDOG
			
			msm_watchdog_suspend(NULL);
#endif
			read_lock(&tasklist_lock);
			do_each_thread(g, p) {
				if (p != current && !freezer_should_skip(p)
				    && freezing(p) && !frozen(p) &&
				    elapsed_msecs > 100)
					sched_show_task(p);
			} while_each_thread(g, p);
			read_unlock(&tasklist_lock);
#ifdef CONFIG_MSM_WATCHDOG
			msm_watchdog_resume(NULL);
#endif
		}
	} else {
		printk("(elapsed %d.%03d seconds) ", elapsed_msecs / 1000,
			elapsed_msecs % 1000);
	}

	return todo ? -EBUSY : 0;
}

int freeze_processes(void)
{
	int error;

	error = __usermodehelper_disable(UMH_FREEZING);
	if (error)
		return error;

	/* Make sure this task doesn't get frozen */
	current->flags |= PF_SUSPEND_TASK;

	if (!pm_freezing)
		atomic_inc(&system_freezing_cnt);

	printk("Freezing user space processes ... ");
	pm_freezing = true;
	error = try_to_freeze_tasks(true);
	if (!error) {
		printk("done.");
		__usermodehelper_set_disable_depth(UMH_DISABLED);
		oom_killer_disable();
	}
	printk("\n");
	BUG_ON(in_atomic());

	if (error)
		thaw_processes();
	return error;
}

int freeze_kernel_threads(void)
{
	int error;

	error = suspend_sys_sync_wait();
	if (error)
		return error;

	printk("Freezing remaining freezable tasks ... ");
	pm_nosig_freezing = true;
	error = try_to_freeze_tasks(false);
	if (!error)
		printk("done.");

	printk("\n");
	BUG_ON(in_atomic());

	if (error)
		thaw_kernel_threads();
	return error;
}

void thaw_processes(void)
{
	struct task_struct *g, *p;
	struct task_struct *curr = current;

	if (pm_freezing)
		atomic_dec(&system_freezing_cnt);
	pm_freezing = false;
	pm_nosig_freezing = false;

	oom_killer_enable();

	printk("Restarting tasks ... ");

	__usermodehelper_set_disable_depth(UMH_FREEZING);
	thaw_workqueues();

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		/* No other threads should have PF_SUSPEND_TASK set */
		WARN_ON((p != curr) && (p->flags & PF_SUSPEND_TASK));
		__thaw_task(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);

	WARN_ON(!(curr->flags & PF_SUSPEND_TASK));
	curr->flags &= ~PF_SUSPEND_TASK;

	usermodehelper_enable();

	schedule();
	printk("done.\n");
}

void thaw_kernel_threads(void)
{
	struct task_struct *g, *p;

	pm_nosig_freezing = false;
	printk("Restarting kernel threads ... ");

	thaw_workqueues();

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		if (p->flags & (PF_KTHREAD | PF_WQ_WORKER))
			__thaw_task(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);

	schedule();
	printk("done.\n");
}
