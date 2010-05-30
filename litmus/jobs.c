/* litmus/jobs.c - common job control code
 */

#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>

void prepare_for_next_period(struct task_struct *t)
{
	BUG_ON(!t);
	/* prepare next release */
	t->rt_param.job_params.release   = t->rt_param.job_params.deadline;
	t->rt_param.job_params.deadline += get_rt_period(t);
	t->rt_param.job_params.exec_time = 0;
	/* update job sequence number */
	t->rt_param.job_params.job_no++;

	/* don't confuse Linux */
	t->rt.time_slice = 1;
}

void release_at(struct task_struct *t, lt_t start)
{
	t->rt_param.job_params.deadline = start;
	prepare_for_next_period(t);
	set_rt_flags(t, RT_F_RUNNING);
}


/*
 *	Deactivate current task until the beginning of the next period.
 */
long complete_job(void)
{
	/* Mark that we do not excute anymore */
	set_rt_flags(current, RT_F_SLEEP);
	/* call schedule, this will return when a new job arrives
	 * it also takes care of preparing for the next release
	 */
	schedule();
	return 0;
}
