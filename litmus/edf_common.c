/*
 * kernel/edf_common.c
 *
 * Common functions for EDF based scheduler.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>


#include <litmus/edf_common.h>

/* edf_higher_prio -  returns true if first has a higher EDF priority
 *                    than second. Deadline ties are broken by PID.
 *
 * first first must not be NULL and a real-time task.
 * second may be NULL or a non-rt task.
 */
int edf_higher_prio(struct task_struct* first,
		    struct task_struct* second)
{
	struct task_struct *first_task = first;
	struct task_struct *second_task = second;

	/* Check for inherited priorities. Change task
	 * used for comparison in such a case.
	 */
	if (first && first->rt_param.inh_task)
		first_task = first->rt_param.inh_task;
	if (second && second->rt_param.inh_task)
		second_task = second->rt_param.inh_task;

	return
		/* does the second task exist and is it a real-time task?  If
		 * not, the first task (which is a RT task) has higher
		 * priority.
		 */
		!second_task || !is_realtime(second_task)  ||

		/* is the deadline of the first task earlier?
		 * Then it has higher priority.
		 */
		earlier_deadline(first_task, second_task) ||

		/* Do we have a deadline tie?
		 * Then break by PID.
		 */
		(get_deadline(first_task) == get_deadline(second_task) &&
	        (first_task->pid < second_task->pid ||

		/* If the PIDs are the same then the task with the inherited
		 * priority wins.
		 */
		(first_task->pid == second_task->pid &&
		 !second->rt_param.inh_task)));
}

int edf_ready_order(struct list_head* a, struct list_head* b)
{
	return edf_higher_prio(
		list_entry(a, struct task_struct, rt_list),
		list_entry(b, struct task_struct, rt_list));
}

void edf_release_at(struct task_struct *t, lt_t start)
{
	t->rt_param.job_params.deadline = start;
	edf_prepare_for_next_period(t);
	set_rt_flags(t, RT_F_RUNNING);
}

void edf_domain_init(rt_domain_t* rt, check_resched_needed_t resched)
{
	rt_domain_init(rt, resched, edf_ready_order);
}

void edf_prepare_for_next_period(struct task_struct *t)
{
	BUG_ON(!t);
	/* prepare next release */
	t->rt_param.job_params.release   = t->rt_param.job_params.deadline;
	t->rt_param.job_params.deadline += get_rt_period(t);
	t->rt_param.job_params.exec_time = 0;
	/* update job sequence number */
	t->rt_param.job_params.job_no++;

	/* don't confuse Linux */
	t->time_slice = 1;
}

/* need_to_preempt - check whether the task t needs to be preempted
 *                   call only with irqs disabled and with  ready_lock acquired
 *                   THIS DOES NOT TAKE NON-PREEMPTIVE SECTIONS INTO ACCOUNT!
 */
int edf_preemption_needed(rt_domain_t* rt, struct task_struct *t)
{
	/* we need the read lock for edf_ready_queue */
	/* no need to preempt if there is nothing pending */
	if (!ready_jobs_pending(rt))
		return 0;
	/* we need to reschedule if t doesn't exist */
	if (!t)
		return 1;

	/* NOTE: We cannot check for non-preemptibility since we
	 *       don't know what address space we're currently in.
	 */

	/* make sure to get non-rt stuff out of the way */
	return !is_realtime(t) || edf_higher_prio(next_ready(rt), t);
}


/*
 *	Deactivate current task until the beginning of the next period.
 */
long edf_complete_job(void)
{
	/* Mark that we do not excute anymore */
	set_rt_flags(current, RT_F_SLEEP);
	/* call schedule, this will return when a new job arrives
	 * it also takes care of preparing for the next release
	 */
	schedule();
	return 0;
}

