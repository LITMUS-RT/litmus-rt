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
 * both first and second may be NULL
 */
int edf_higher_prio(struct task_struct* first,
		    struct task_struct* second)
{
	struct task_struct *first_task = first;
	struct task_struct *second_task = second;

	/* There is no point in comparing a task to itself. */
	if (first && first == second) {
		TRACE_TASK(first,
			   "WARNING: pointless edf priority comparison.\n");
		return 0;
	}


	/* Check for inherited priorities. Change task
	 * used for comparison in such a case.
	 */
	if (first && first->rt_param.inh_task)
		first_task = first->rt_param.inh_task;
	if (second && second->rt_param.inh_task)
		second_task = second->rt_param.inh_task;

	return
		/* it has to exist in order to have higher priority */
		first_task && (
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
		 !second->rt_param.inh_task))));
}

int edf_ready_order(struct bheap_node* a, struct bheap_node* b)
{
	return edf_higher_prio(bheap2task(a), bheap2task(b));
}

void edf_domain_init(rt_domain_t* rt, check_resched_needed_t resched,
		      release_jobs_t release)
{
	rt_domain_init(rt,  edf_ready_order, resched, release);
}

/* need_to_preempt - check whether the task t needs to be preempted
 *                   call only with irqs disabled and with  ready_lock acquired
 *                   THIS DOES NOT TAKE NON-PREEMPTIVE SECTIONS INTO ACCOUNT!
 */
int edf_preemption_needed(rt_domain_t* rt, struct task_struct *t)
{
	/* we need the read lock for edf_ready_queue */
	/* no need to preempt if there is nothing pending */
	if (!__jobs_pending(rt))
		return 0;
	/* we need to reschedule if t doesn't exist */
	if (!t)
		return 1;

	/* NOTE: We cannot check for non-preemptibility since we
	 *       don't know what address space we're currently in.
	 */

	/* make sure to get non-rt stuff out of the way */
	return !is_realtime(t) || edf_higher_prio(__next_ready(rt), t);
}
