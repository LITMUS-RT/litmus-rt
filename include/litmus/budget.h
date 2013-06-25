#ifndef _LITMUS_BUDGET_H_
#define _LITMUS_BUDGET_H_

/* Update the per-processor enforcement timer (arm/reproram/cancel) for
 * the next task. */
void update_enforcement_timer(struct task_struct* t);

inline static int budget_exhausted(struct task_struct* t)
{
	return get_exec_time(t) >= get_exec_cost(t);
}

inline static lt_t budget_remaining(struct task_struct* t)
{
	if (!budget_exhausted(t))
		return get_exec_cost(t) - get_exec_time(t);
	else
		/* avoid overflow */
		return 0;
}

#define budget_enforced(t) (tsk_rt(t)->task_params.budget_policy != NO_ENFORCEMENT)

#define budget_precisely_enforced(t) (tsk_rt(t)->task_params.budget_policy \
				      == PRECISE_ENFORCEMENT)

static inline int requeue_preempted_job(struct task_struct* t)
{
	/* Add task to ready queue only if not subject to budget enforcement or
	 * if the job has budget remaining. t may be NULL.
	 */
	return t && !is_completed(t) &&
		(!budget_exhausted(t) || !budget_enforced(t));
}

#endif
