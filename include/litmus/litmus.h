/*
 * Constant definitions related to
 * scheduling policy.
 */

#ifndef _LINUX_LITMUS_H_
#define _LINUX_LITMUS_H_

#include <litmus/debug_trace.h>

#ifdef CONFIG_RELEASE_MASTER
extern atomic_t release_master_cpu;
#endif

/* in_list - is a given list_head queued on some list?
 */
static inline int in_list(struct list_head* list)
{
	return !(  /* case 1: deleted */
		   (list->next == LIST_POISON1 &&
		    list->prev == LIST_POISON2)
		 ||
		   /* case 2: initialized */
		   (list->next == list &&
		    list->prev == list)
		);
}

#define NO_CPU			0xffffffff

void litmus_fork(struct task_struct *tsk);
void litmus_exec(void);
/* clean up real-time state of a task */
void exit_litmus(struct task_struct *dead_tsk);

long litmus_admit_task(struct task_struct *tsk);
void litmus_exit_task(struct task_struct *tsk);

#define is_realtime(t) 		((t)->policy == SCHED_LITMUS)
#define rt_transition_pending(t) \
	((t)->rt_param.transition_pending)

#define tsk_rt(t)		(&(t)->rt_param)

/*	Realtime utility macros */
#define get_rt_flags(t)		(tsk_rt(t)->flags)
#define set_rt_flags(t,f) 	(tsk_rt(t)->flags=(f))
#define get_exec_cost(t)  	(tsk_rt(t)->task_params.exec_cost)
#define get_exec_time(t)	(tsk_rt(t)->job_params.exec_time)
#define get_rt_period(t)	(tsk_rt(t)->task_params.period)
#define get_rt_phase(t)		(tsk_rt(t)->task_params.phase)
#define get_partition(t) 	(tsk_rt(t)->task_params.cpu)
#define get_deadline(t)		(tsk_rt(t)->job_params.deadline)
#define get_release(t)		(tsk_rt(t)->job_params.release)
#define get_class(t)		(tsk_rt(t)->task_params.cls)

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

#define is_hrt(t)     		\
	(tsk_rt(t)->task_params.class == RT_CLASS_HARD)
#define is_srt(t)     		\
	(tsk_rt(t)->task_params.class == RT_CLASS_SOFT)
#define is_be(t)      		\
	(tsk_rt(t)->task_params.class == RT_CLASS_BEST_EFFORT)

/* Our notion of time within LITMUS: kernel monotonic time. */
static inline lt_t litmus_clock(void)
{
	return ktime_to_ns(ktime_get());
}

/* A macro to convert from nanoseconds to ktime_t. */
#define ns_to_ktime(t)		ktime_add_ns(ktime_set(0, 0), t)

#define get_domain(t) (tsk_rt(t)->domain)

/* Honor the flag in the preempt_count variable that is set
 * when scheduling is in progress.
 */
#define is_running(t) 			\
	((t)->state == TASK_RUNNING || 	\
	 task_thread_info(t)->preempt_count & PREEMPT_ACTIVE)

#define is_blocked(t)       \
	(!is_running(t))
#define is_released(t, now)	\
	(lt_before_eq(get_release(t), now))
#define is_tardy(t, now)    \
	(lt_before_eq(tsk_rt(t)->job_params.deadline, now))

/* real-time comparison macros */
#define earlier_deadline(a, b) (lt_before(\
	(a)->rt_param.job_params.deadline,\
	(b)->rt_param.job_params.deadline))
#define earlier_release(a, b)  (lt_before(\
	(a)->rt_param.job_params.release,\
	(b)->rt_param.job_params.release))

void preempt_if_preemptable(struct task_struct* t, int on_cpu);

#ifdef CONFIG_SRP
void srp_ceiling_block(void);
#else
#define srp_ceiling_block() /* nothing */
#endif

#define bheap2task(hn) ((struct task_struct*) hn->value)

#ifdef CONFIG_NP_SECTION

static inline int is_kernel_np(struct task_struct *t)
{
	return tsk_rt(t)->kernel_np;
}

static inline int is_user_np(struct task_struct *t)
{
	return tsk_rt(t)->ctrl_page ? tsk_rt(t)->ctrl_page->np_flag : 0;
}

static inline void request_exit_np(struct task_struct *t)
{
	if (is_user_np(t)) {
		/* Set the flag that tells user space to call
		 * into the kernel at the end of a critical section. */
		if (likely(tsk_rt(t)->ctrl_page)) {
			TRACE_TASK(t, "setting delayed_preemption flag\n");
			tsk_rt(t)->ctrl_page->delayed_preemption = 1;
		}
	}
}

static inline void clear_exit_np(struct task_struct *t)
{
	if (likely(tsk_rt(t)->ctrl_page))
		tsk_rt(t)->ctrl_page->delayed_preemption = 0;
}

static inline void make_np(struct task_struct *t)
{
	tsk_rt(t)->kernel_np++;
}

/* Caller should check if preemption is necessary when
 * the function return 0.
 */
static inline int take_np(struct task_struct *t)
{
	return --tsk_rt(t)->kernel_np;
}

#else

static inline int is_kernel_np(struct task_struct* t)
{
	return 0;
}

static inline int is_user_np(struct task_struct* t)
{
	return 0;
}

static inline void request_exit_np(struct task_struct *t)
{
	/* request_exit_np() shouldn't be called if !CONFIG_NP_SECTION */
	BUG();
}

static inline void clear_exit_np(struct task_struct* t)
{
}

#endif

static inline int is_np(struct task_struct *t)
{
#ifdef CONFIG_SCHED_DEBUG_TRACE
	int kernel, user;
	kernel = is_kernel_np(t);
	user   = is_user_np(t);
	if (kernel || user)
		TRACE_TASK(t, " is non-preemptive: kernel=%d user=%d\n",

			   kernel, user);
	return kernel || user;
#else
	return unlikely(is_kernel_np(t) || is_user_np(t));
#endif
}

static inline int is_present(struct task_struct* t)
{
	return t && tsk_rt(t)->present;
}


/* make the unit explicit */
typedef unsigned long quanta_t;

enum round {
	FLOOR,
	CEIL
};


/* Tick period is used to convert ns-specified execution
 * costs and periods into tick-based equivalents.
 */
extern ktime_t tick_period;

static inline quanta_t time2quanta(lt_t time, enum round round)
{
	s64  quantum_length = ktime_to_ns(tick_period);

	if (do_div(time, quantum_length) && round == CEIL)
		time++;
	return (quanta_t) time;
}

/* By how much is cpu staggered behind CPU 0? */
u64 cpu_stagger_offset(int cpu);

#endif
