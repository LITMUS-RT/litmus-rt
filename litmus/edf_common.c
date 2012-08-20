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


	/* check for NULL tasks */
	if (!first || !second)
		return first && !second;

#ifdef CONFIG_LITMUS_LOCKING

	/* Check for inherited priorities. Change task
	 * used for comparison in such a case.
	 */
	if (unlikely(first->rt_param.inh_task))
		first_task = first->rt_param.inh_task;
	if (unlikely(second->rt_param.inh_task))
		second_task = second->rt_param.inh_task;

	/* Check for priority boosting. Tie-break by start of boosting.
	 */
	if (unlikely(is_priority_boosted(first_task))) {
		/* first_task is boosted, how about second_task? */
		if (!is_priority_boosted(second_task) ||
		    lt_before(get_boost_start(first_task),
			      get_boost_start(second_task)))
			return 1;
		else
			return 0;
	} else if (unlikely(is_priority_boosted(second_task)))
		/* second_task is boosted, first is not*/
		return 0;

#endif

	/* Determine the task with earliest deadline, with
	 * tie-break logic.
	 */
	if (unlikely(!is_realtime(second_task))) {
		return 1;
	}
	else if (earlier_deadline(first_task, second_task)) {
		/* Is the deadline of the first task earlier?
		 * Then it has higher priority.
		 */
		return 1;
	}
	else if (get_deadline(first_task) == get_deadline(second_task)) {
		/* Need to tie break */

		/* Tie break by pid */
		if (first_task->pid < second_task->pid) {
			return 1;
		}
		else if (first_task->pid == second_task->pid) {
			/* If the PIDs are the same then the task with the
			 * inherited priority wins.
			 */
			if (!second->rt_param.inh_task) {
				return 1;
			}
		}
	}
	return 0; /* fall-through. prio(second_task) > prio(first_task) */
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
