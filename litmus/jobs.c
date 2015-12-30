/* litmus/jobs.c - common job control code
 */

#include <linux/sched.h>

#include <litmus/preempt.h>
#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>
#include <litmus/jobs.h>

static inline void setup_release(struct task_struct *t, lt_t release)
{
	/* prepare next release */
	t->rt_param.job_params.release = release;
	t->rt_param.job_params.deadline = release + get_rt_relative_deadline(t);
	t->rt_param.job_params.exec_time = 0;

	/* update job sequence number */
	t->rt_param.job_params.job_no++;

	/* expose to user space */
	if (has_control_page(t)) {
		struct control_page* cp = get_control_page(t);
		cp->deadline = t->rt_param.job_params.deadline;
		cp->release = get_release(t);
		cp->job_index = t->rt_param.job_params.job_no;
	}
}

void prepare_for_next_period(struct task_struct *t)
{
	BUG_ON(!t);

	/* Record lateness before we set up the next job's
	 * release and deadline. Lateness may be negative.
	 */
	t->rt_param.job_params.lateness =
		(long long)litmus_clock() -
		(long long)t->rt_param.job_params.deadline;

	if (tsk_rt(t)->sporadic_release) {
		TRACE_TASK(t, "sporadic release at %llu\n",
			   tsk_rt(t)->sporadic_release_time);
		/* sporadic release */
		setup_release(t, tsk_rt(t)->sporadic_release_time);
		tsk_rt(t)->sporadic_release = 0;
	} else {
		/* periodic release => add period */
		setup_release(t, get_release(t) + get_rt_period(t));
	}
}

void release_at(struct task_struct *t, lt_t start)
{
	BUG_ON(!t);
	setup_release(t, start);
	tsk_rt(t)->completed = 0;
}

long default_wait_for_release_at(lt_t release_time)
{
	struct task_struct *t = current;
	unsigned long flags;

	local_irq_save(flags);
	tsk_rt(t)->sporadic_release_time = release_time;
	smp_wmb();
	tsk_rt(t)->sporadic_release = 1;
	local_irq_restore(flags);

	return litmus->complete_job();
}


/*
 *	Deactivate current task until the beginning of the next period.
 */
long complete_job(void)
{
	preempt_disable();
	TRACE_CUR("job completion indicated at %llu\n", litmus_clock());
	/* Mark that we do not excute anymore */
	tsk_rt(current)->completed = 1;
	/* call schedule, this will return when a new job arrives
	 * it also takes care of preparing for the next release
	 */
	litmus_reschedule_local();
	preempt_enable();
	return 0;
}

static long sleep_until_next_release(void);

/* alternative job completion implementation that suspends the task */
long complete_job_oneshot(void)
{
	struct task_struct *t = current;

	preempt_disable();

	TRACE_CUR("job completes at %llu (deadline: %llu)\n", litmus_clock(),
		get_deadline(t));

	sched_trace_task_completion(t, 0);
	prepare_for_next_period(t);
	sched_trace_task_release(t);

	return sleep_until_next_release();
}

/* assumes caller has disabled preemptions;
 * re-enables preemptions before returning */
static long sleep_until_next_release(void)
{
	struct task_struct *t = current;
	ktime_t next_release;
	long err;

	next_release = ns_to_ktime(get_release(t));

	TRACE_CUR("next_release=%llu\n", get_release(t));

	if (lt_after(get_release(t), litmus_clock())) {
		set_current_state(TASK_INTERRUPTIBLE);
		tsk_rt(t)->completed = 1;
		preempt_enable_no_resched();
		err = schedule_hrtimeout(&next_release, HRTIMER_MODE_ABS);
		/* If we get woken by a signal, we return early.
		 * This is intentional; we want to be able to kill tasks
		 * that are waiting for the next job release.
		 */
		tsk_rt(t)->completed = 0;
	} else {
		err = 0;
		TRACE_CUR("TARDY: release=%llu now=%llu\n", get_release(t), litmus_clock());
		preempt_enable();
	}

	TRACE_CUR("return to next job at %llu\n", litmus_clock());
	return err;
}
