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

#ifdef CONFIG_EDF_TIE_BREAK_LATENESS_NORM
#include <litmus/fpmath.h>
#endif

#ifdef CONFIG_EDF_TIE_BREAK_HASH
#include <linux/hash.h>
static inline long edf_hash(struct task_struct *t)
{
	/* pid is 32 bits, so normally we would shove that into the
	 * upper 32-bits and and put the job number in the bottom
	 * and hash the 64-bit number with hash_64(). Sadly,
	 * in testing, hash_64() doesn't distribute keys were the
	 * upper bits are close together (as would be the case with
	 * pids) and job numbers are equal (as would be the case with
	 * synchronous task sets with all relative deadlines equal).
	 *
	 * A 2006 Linux patch proposed the following solution
	 * (but for some reason it wasn't accepted...).
	 *
	 * At least this workaround works for 32-bit systems as well.
	 */
	return hash_32(hash_32((u32)tsk_rt(t)->job_params.job_no, 32) ^ t->pid, 32);
}
#endif


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

	if (earlier_deadline(first_task, second_task)) {
		return 1;
	}
	else if (get_deadline(first_task) == get_deadline(second_task)) {
		/* Need to tie break. All methods must set pid_break to 0/1 if
		 * first_task does not have priority over second_task.
		 */
		int pid_break;


#if defined(CONFIG_EDF_TIE_BREAK_LATENESS)
		/* Tie break by lateness. Jobs with greater lateness get
		 * priority. This should spread tardiness across all tasks,
		 * especially in task sets where all tasks have the same
		 * period and relative deadlines.
		 */
		if (get_lateness(first_task) > get_lateness(second_task)) {
			return 1;
		}
		pid_break = (get_lateness(first_task) == get_lateness(second_task));


#elif defined(CONFIG_EDF_TIE_BREAK_LATENESS_NORM)
		/* Tie break by lateness, normalized by relative deadline. Jobs with
		 * greater normalized lateness get priority.
		 *
		 * Note: Considered using the algebraically equivalent
		 *	lateness(first)*relative_deadline(second) >
					lateness(second)*relative_deadline(first)
		 * to avoid fixed-point math, but values are prone to overflow if inputs
		 * are on the order of several seconds, even in 64-bit.
		 */
		fp_t fnorm = _frac(get_lateness(first_task),
						   get_rt_relative_deadline(first_task));
		fp_t snorm = _frac(get_lateness(second_task),
						   get_rt_relative_deadline(second_task));
		if (_gt(fnorm, snorm)) {
			return 1;
		}
		pid_break = _eq(fnorm, snorm);


#elif defined(CONFIG_EDF_TIE_BREAK_HASH)
		/* Tie break by comparing hashs of (pid, job#) tuple.  There should be
		 * a 50% chance that first_task has a higher priority than second_task.
		 */
		long fhash = edf_hash(first_task);
		long shash = edf_hash(second_task);
		if (fhash < shash) {
			return 1;
		}
		pid_break = (fhash == shash);
#else


		/* CONFIG_EDF_PID_TIE_BREAK */
		pid_break = 1; // fall through to tie-break by pid;
#endif

		/* Tie break by pid */
		if(pid_break) {
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
