/* litmus/sync.c - Support for synchronous and asynchronous task system releases.
 *
 *
 */

#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/completion.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>

static DECLARE_COMPLETION(ts_release);

static long do_wait_for_ts_release(void)
{
	long ret = 0;

	/* If the interruption races with a release, the completion object
	 * may have a non-zero counter. To avoid this problem, this should
	 * be replaced by wait_for_completion().
	 *
	 * For debugging purposes, this is interruptible for now.
	 */
	ret = wait_for_completion_interruptible(&ts_release);

	return ret;
}


static long do_release_ts(lt_t start)
{
	long ret = 0;
	int  task_count = 0;
	long flags;
	struct list_head	*pos;
	struct task_struct 	*t;


	spin_lock_irqsave(&ts_release.wait.lock, flags);

	list_for_each(pos, &ts_release.wait.task_list) {
		t = (struct task_struct*) list_entry(pos,
						     struct __wait_queue,
						     task_list)->private;
		task_count++;
		release_at(t, start + t->rt_param.task_params.phase);
	}

	spin_unlock_irqrestore(&ts_release.wait.lock, flags);

	complete_n(&ts_release, task_count);

	return ret;
}


asmlinkage long sys_wait_for_ts_release(void)
{
	long ret = -EPERM;
	struct task_struct *t = current;

	if (is_realtime(t))
		ret = do_wait_for_ts_release();

	return ret;
}


asmlinkage long sys_release_ts(lt_t __user *__delay)
{
	long ret;
	lt_t delay;

	/* FIXME: check capabilities... */

	ret = copy_from_user(&delay, __delay, sizeof(lt_t));
	if (ret == 0)
		ret = do_release_ts(sched_clock() + delay);

	return ret;
}
