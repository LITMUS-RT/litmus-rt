/*
 * Definition of the scheduler plugin interface.
 *
 */
#ifndef _LINUX_RT_PARAM_H_
#define _LINUX_RT_PARAM_H_

/* Litmus time type. */
typedef unsigned long long lt_t;

static inline int lt_after(lt_t a, lt_t b)
{
	return ((long long) b) - ((long long) a) < 0;
}
#define lt_before(a, b) lt_after(b, a)

static inline int lt_after_eq(lt_t a, lt_t b)
{
	return ((long long) a) - ((long long) b) >= 0;
}
#define lt_before_eq(a, b) lt_after_eq(b, a)

/* different types of clients */
typedef enum {
	RT_CLASS_HARD,
	RT_CLASS_SOFT,
	RT_CLASS_BEST_EFFORT
} task_class_t;

typedef enum {
	NO_ENFORCEMENT,      /* job may overrun unhindered */
	QUANTUM_ENFORCEMENT, /* budgets are only checked on quantum boundaries */
	PRECISE_ENFORCEMENT  /* budgets are enforced with hrtimers */
} budget_policy_t;

/* Release behaviors for jobs. PERIODIC and EARLY jobs
   must end by calling sys_complete_job() (or equivalent)
   to set up their next release and deadline. */
typedef enum {
	/* Jobs are released sporadically (provided job precedence
       constraints are met). */
	TASK_SPORADIC,

	/* Jobs are released periodically (provided job precedence
       constraints are met). */
	TASK_PERIODIC,

    /* Jobs are released immediately after meeting precedence
       constraints. Beware this can peg your CPUs if used in
       the wrong applications. Only supported by EDF schedulers. */
	TASK_EARLY
} release_policy_t;

/* We use the common priority interpretation "lower index == higher priority",
 * which is commonly used in fixed-priority schedulability analysis papers.
 * So, a numerically lower priority value implies higher scheduling priority,
 * with priority 1 being the highest priority. Priority 0 is reserved for
 * priority boosting. LITMUS_MAX_PRIORITY denotes the maximum priority value
 * range.
 */

#define LITMUS_MAX_PRIORITY     512
#define LITMUS_HIGHEST_PRIORITY   1
#define LITMUS_LOWEST_PRIORITY    (LITMUS_MAX_PRIORITY - 1)
#define LITMUS_NO_PRIORITY	UINT_MAX

/* Provide generic comparison macros for userspace,
 * in case that we change this later. */
#define litmus_higher_fixed_prio(a, b)	(a < b)
#define litmus_lower_fixed_prio(a, b)	(a > b)
#define litmus_is_valid_fixed_prio(p)		\
	((p) >= LITMUS_HIGHEST_PRIORITY &&	\
	 (p) <= LITMUS_LOWEST_PRIORITY)

/* reservation support */

typedef enum {
	PERIODIC_POLLING = 10,
	SPORADIC_POLLING,
	TABLE_DRIVEN,
} reservation_type_t;

struct lt_interval {
	lt_t start;
	lt_t end;
};

#ifndef __KERNEL__
#define __user
#endif

struct reservation_config {
	unsigned int id;
	lt_t priority;
	int  cpu;

	union {
		struct {
			lt_t period;
			lt_t budget;
			lt_t relative_deadline;
			lt_t offset;
		} polling_params;

		struct {
			lt_t major_cycle_length;
			unsigned int num_intervals;
			struct lt_interval __user *intervals;
		} table_driven_params;
	};
};

/* regular sporadic task support */

struct rt_task {
	lt_t 		exec_cost;
	lt_t 		period;
	lt_t		relative_deadline;
	lt_t		phase;
	unsigned int	cpu;
	unsigned int	priority;
	task_class_t	cls;
	budget_policy_t  budget_policy;  /* ignored by pfair */
	release_policy_t release_policy;
};

/* don't export internal data structures to user space (liblitmus) */
#ifdef __KERNEL__

struct _rt_domain;
struct bheap_node;
struct release_heap;

struct rt_job {
	/* Time instant the the job was or will be released.  */
	lt_t	release;

	/* What is the current deadline? */
	lt_t   	deadline;

	/* How much service has this job received so far? */
	lt_t	exec_time;

	/* By how much did the prior job miss its deadline by?
	 * Value differs from tardiness in that lateness may
	 * be negative (when job finishes before its deadline).
	 */
	long long	lateness;

	/* Which job is this. This is used to let user space
	 * specify which job to wait for, which is important if jobs
	 * overrun. If we just call sys_sleep_next_period() then we
	 * will unintentionally miss jobs after an overrun.
	 *
	 * Increase this sequence number when a job is released.
	 */
	unsigned int    job_no;

#ifdef CONFIG_SCHED_TASK_TRACE
	/* Keep track of the last time the job suspended.
	 * -> used for tracing sporadic tasks. */
	lt_t	last_suspension;
#endif
};

struct pfair_param;

/*	RT task parameters for scheduling extensions
 *	These parameters are inherited during clone and therefore must
 *	be explicitly set up before the task set is launched.
 */
struct rt_param {
	/* do we need to check for srp blocking? */
	unsigned int		srp_non_recurse:1;

	/* is the task present? (true if it can be scheduled) */
	unsigned int		present:1;

	/* has the task completed? */
	unsigned int		completed:1;

#ifdef CONFIG_LITMUS_LOCKING
	/* Is the task being priority-boosted by a locking protocol? */
	unsigned int		priority_boosted:1;
	/* If so, when did this start? */
	lt_t			boost_start_time;

	/* How many LITMUS^RT locks does the task currently hold/wait for? */
	unsigned int		num_locks_held;
	/* How many PCP/SRP locks does the task currently hold/wait for? */
	unsigned int		num_local_locks_held;
#endif

	/* user controlled parameters */
	struct rt_task 		task_params;

	/* timing parameters */
	struct rt_job 		job_params;


	/* Special handling for periodic tasks executing
	 * clock_nanosleep(CLOCK_MONOTONIC, ...).
	 */
	lt_t			nanosleep_wakeup;
	unsigned int	doing_abs_nanosleep:1;

	/* Should the next job be released at some time other than
	 * just period time units after the last release?
	 */
	unsigned int		sporadic_release:1;
	lt_t			sporadic_release_time;

	/* task representing the current "inherited" task
	 * priority, assigned by inherit_priority and
	 * return priority in the scheduler plugins.
	 * could point to self if PI does not result in
	 * an increased task priority.
	 */
	 struct task_struct*	inh_task;

#ifdef CONFIG_NP_SECTION
	/* For the FMLP under PSN-EDF, it is required to make the task
	 * non-preemptive from kernel space. In order not to interfere with
	 * user space, this counter indicates the kernel space np setting.
	 * kernel_np > 0 => task is non-preemptive
	 */
	unsigned int	kernel_np;
#endif

	/* This field can be used by plugins to store where the task
	 * is currently scheduled. It is the responsibility of the
	 * plugin to avoid race conditions.
	 *
	 * This used by GSN-EDF and PFAIR.
	 */
	volatile int		scheduled_on;

	/* Is the stack of the task currently in use? This is updated by
	 * the LITMUS core.
	 *
	 * Be careful to avoid deadlocks!
	 */
	volatile int		stack_in_use;

	/* This field can be used by plugins to store where the task
	 * is currently linked. It is the responsibility of the plugin
	 * to avoid race conditions.
	 *
	 * Used by GSN-EDF.
	 */
	volatile int		linked_on;

	/* PFAIR/PD^2 state. Allocated on demand. */
	union {
		void *plugin_state;
		struct pfair_param *pfair;
	};

	/* Fields saved before BE->RT transition.
	 */
	int old_policy;
	int old_prio;

	/* ready queue for this task */
	struct _rt_domain* domain;

	/* heap element for this task
	 *
	 * Warning: Don't statically allocate this node. The heap
	 *          implementation swaps these between tasks, thus after
	 *          dequeuing from a heap you may end up with a different node
	 *          then the one you had when enqueuing the task.  For the same
	 *          reason, don't obtain and store references to this node
	 *          other than this pointer (which is updated by the heap
	 *          implementation).
	 */
	struct bheap_node*	heap_node;
	struct release_heap*	rel_heap;

	/* Used by rt_domain to queue task in release list.
	 */
	struct list_head list;

	/* Pointer to the page shared between userspace and kernel. */
	struct control_page * ctrl_page;
};

#endif

#endif
