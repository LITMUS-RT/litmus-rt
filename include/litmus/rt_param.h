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
	PRECISE_ENFORCEMENT  /* NOT IMPLEMENTED - enforced with hrtimers */
} budget_policy_t;

struct rt_task {
	lt_t 		exec_cost;
	lt_t 		period;
	lt_t		phase;
	unsigned int	cpu;
	task_class_t	cls;
	budget_policy_t budget_policy; /* ignored by pfair */
};

/* The definition of the data that is shared between the kernel and real-time
 * tasks via a shared page (see litmus/ctrldev.c).
 *
 * WARNING: User space can write to this, so don't trust
 * the correctness of the fields!
 *
 * This servees two purposes: to enable efficient signaling
 * of non-preemptive sections (user->kernel) and
 * delayed preemptions (kernel->user), and to export
 * some real-time relevant statistics such as preemption and
 * migration data to user space. We can't use a device to export
 * statistics because we want to avoid system call overhead when
 * determining preemption/migration overheads).
 */
struct control_page {
	/* Is the task currently in a non-preemptive section? */
	int np_flag;
	/* Should the task call into the kernel when it leaves
	 * its non-preemptive section? */
	int delayed_preemption;

	/* to be extended */
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

	/* Which job is this. This is used to let user space
	 * specify which job to wait for, which is important if jobs
	 * overrun. If we just call sys_sleep_next_period() then we
	 * will unintentionally miss jobs after an overrun.
	 *
	 * Increase this sequence number when a job is released.
	 */
	unsigned int    job_no;
};

struct pfair_param;

/*	RT task parameters for scheduling extensions
 *	These parameters are inherited during clone and therefore must
 *	be explicitly set up before the task set is launched.
 */
struct rt_param {
	/* is the task sleeping? */
	unsigned int 		flags:8;

	/* do we need to check for srp blocking? */
	unsigned int		srp_non_recurse:1;

	/* is the task present? (true if it can be scheduled) */
	unsigned int		present:1;

	/* user controlled parameters */
	struct rt_task 		task_params;

	/* timing parameters */
	struct rt_job 		job_params;

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
	struct pfair_param*	pfair;

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

/*	Possible RT flags	*/
#define RT_F_RUNNING		0x00000000
#define RT_F_SLEEP		0x00000001
#define RT_F_EXIT_SEM		0x00000008

#endif

#endif
