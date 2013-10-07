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

struct task_struct* __waitqueue_remove_first(wait_queue_head_t *wq);

#define NO_CPU			0xffffffff

void litmus_fork(struct task_struct *tsk);
void litmus_exec(void);
/* clean up real-time state of a task */
void litmus_clear_state(struct task_struct *dead_tsk);
void exit_litmus(struct task_struct *dead_tsk);

long litmus_admit_task(struct task_struct *tsk);
void litmus_exit_task(struct task_struct *tsk);
void litmus_dealloc(struct task_struct *tsk);
void litmus_do_exit(struct task_struct *tsk);

#define is_realtime(t) 		((t)->policy == SCHED_LITMUS)
#define rt_transition_pending(t) \
	((t)->rt_param.transition_pending)

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
#define get_release_policy(t) (tsk_rt(t)->task_params.release_policy)

/* job_param macros */
#define get_exec_time(t)    (tsk_rt(t)->job_params.exec_time)
#define get_deadline(t)		(tsk_rt(t)->job_params.deadline)
#define get_release(t)		(tsk_rt(t)->job_params.release)
#define get_lateness(t)		(tsk_rt(t)->job_params.lateness)

/* release policy macros */
#define is_periodic(t)		(get_release_policy(t) == TASK_PERIODIC)
#define is_sporadic(t)		(get_release_policy(t) == TASK_SPORADIC)
#ifdef CONFIG_ALLOW_EARLY_RELEASE
#define is_early_releasing(t)	(get_release_policy(t) == TASK_EARLY)
#else
#define is_early_releasing(t)	(0)
#endif

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

#ifdef CONFIG_LITMUS_LOCKING
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
	return tsk_rt(t)->ctrl_page ? tsk_rt(t)->ctrl_page->sched.np.flag : 0;
}

static inline void request_exit_np(struct task_struct *t)
{
	if (is_user_np(t)) {
		/* Set the flag that tells user space to call
		 * into the kernel at the end of a critical section. */
		if (likely(tsk_rt(t)->ctrl_page)) {
			TRACE_TASK(t, "setting delayed_preemption flag\n");
			tsk_rt(t)->ctrl_page->sched.np.preempt = 1;
		}
	}
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

/* returns 0 if remote CPU needs an IPI to preempt, 1 if no IPI is required */
static inline int request_exit_np_atomic(struct task_struct *t)
{
	union np_flag old, new;

	if (tsk_rt(t)->ctrl_page) {
		old.raw = tsk_rt(t)->ctrl_page->sched.raw;
		if (old.np.flag == 0) {
			/* no longer non-preemptive */
			return 0;
		} else if (old.np.preempt) {
			/* already set, nothing for us to do */
			return 1;
		} else {
			/* non preemptive and flag not set */
			new.raw = old.raw;
			new.np.preempt = 1;
			/* if we get old back, then we atomically set the flag */
			return cmpxchg(&tsk_rt(t)->ctrl_page->sched.raw, old.raw, new.raw) == old.raw;
			/* If we raced with a concurrent change, then so be
			 * it. Deliver it by IPI.  We don't want an unbounded
			 * retry loop here since tasks might exploit that to
			 * keep the kernel busy indefinitely. */
		}
	} else
		return 0;
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

static inline int request_exit_np_atomic(struct task_struct *t)
{
	return 0;
}

#endif

static inline void clear_exit_np(struct task_struct *t)
{
	if (likely(tsk_rt(t)->ctrl_page))
		tsk_rt(t)->ctrl_page->sched.np.preempt = 0;
}

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

static inline int is_completed(struct task_struct* t)
{
	return t && tsk_rt(t)->completed;
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

static inline struct control_page* get_control_page(struct task_struct *t)
{
	return tsk_rt(t)->ctrl_page;
}

static inline int has_control_page(struct task_struct* t)
{
	return tsk_rt(t)->ctrl_page != NULL;
}


#ifdef CONFIG_SCHED_OVERHEAD_TRACE

#define TS_SYSCALL_IN_START						\
	if (has_control_page(current)) {				\
		__TS_SYSCALL_IN_START(&get_control_page(current)->ts_syscall_start); \
	}

#define TS_SYSCALL_IN_END						\
	if (has_control_page(current)) {				\
		uint64_t irqs;						\
		local_irq_disable();					\
		irqs = get_control_page(current)->irq_count -		\
			get_control_page(current)->irq_syscall_start;	\
		__TS_SYSCALL_IN_END(&irqs);				\
		local_irq_enable();					\
	}

#else

#define TS_SYSCALL_IN_START
#define TS_SYSCALL_IN_END

#endif

#endif
