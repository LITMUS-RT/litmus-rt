/*
 * Constant definitions related to
 * scheduling policy.
 */

#ifndef _LINUX_LITMUS_H_
#define _LINUX_LITMUS_H_

#define is_realtime(t) 		((t)->policy == SCHED_LITMUS)

#define tsk_rt(t)		(&(t)->rt_param)

/*	Realtime utility macros */
#ifdef CONFIG_LITMUS_LOCKING
#define is_priority_boosted(t)  (tsk_rt(t)->priority_boosted)
#define get_boost_start(t)  (tsk_rt(t)->boost_start_time)
#else
#define is_priority_boosted(t)  0
#define get_boost_start(t)      0
#endif


/* task_params macros */
#define get_exec_cost(t)  	(tsk_rt(t)->task_params.exec_cost)
#define get_rt_period(t)	(tsk_rt(t)->task_params.period)
#define get_rt_relative_deadline(t)	(tsk_rt(t)->task_params.relative_deadline)
#define get_rt_phase(t)		(tsk_rt(t)->task_params.phase)
#define get_partition(t) 	(tsk_rt(t)->task_params.cpu)
#define get_priority(t) 	(tsk_rt(t)->task_params.priority)
#define get_class(t)        (tsk_rt(t)->task_params.cls)

/* job_param macros */
#define get_exec_time(t)    (tsk_rt(t)->job_params.exec_time)
#define get_deadline(t)		(tsk_rt(t)->job_params.deadline)
#define get_release(t)		(tsk_rt(t)->job_params.release)
#define get_lateness(t)		(tsk_rt(t)->job_params.lateness)

#define is_hrt(t)     		\
	(tsk_rt(t)->task_params.cls == RT_CLASS_HARD)
#define is_srt(t)     		\
	(tsk_rt(t)->task_params.cls == RT_CLASS_SOFT)
#define is_be(t)      		\
	(tsk_rt(t)->task_params.cls == RT_CLASS_BEST_EFFORT)

/* Our notion of time within LITMUS: kernel monotonic time. */
static inline lt_t litmus_clock(void)
{
	return ktime_to_ns(ktime_get());
}

static inline struct control_page* get_control_page(struct task_struct *t)
{
	return tsk_rt(t)->ctrl_page;
}

static inline int has_control_page(struct task_struct* t)
{
	return tsk_rt(t)->ctrl_page != NULL;
}

#endif
