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

struct rt_task {
	lt_t 		exec_cost;
	lt_t 		period;
	lt_t		phase;
	unsigned int  	cpu;
	task_class_t  	cls;
};

/* don't export internal data structures to user space (liblitmus) */
#ifdef __KERNEL__

struct rt_job {
	/* Time instant the the job was or will be released.  */
	lt_t	release;
	/* What is the current deadline? */
	lt_t   	deadline;
	/* How much service has this job received so far?
	 */
	lt_t	exec_time;

	/* Which job is this. This is used to let user space
	 * specify which job to wait for, which is important if jobs
	 * overrun. If we just call sys_sleep_next_period() then we
	 * will unintentionally miss jobs after an overrun.
	 *
	 * Increase this sequence number when a job is released.
	 */
	unsigned int    job_no;

	/* when did this job start executing? */
	lt_t	exec_start;
};


/*	RT task parameters for scheduling extensions
 *	These parameters are inherited during clone and therefore must
 *	be explicitly set up before the task set is launched.
 */
struct rt_param {
	/* is the task sleeping? */
	unsigned int 		flags:8;

	/* Did this task register any SRP controlled resource accesses?
	 * This, of course, should only ever be true under partitioning.
	 * However, this limitation is not currently enforced.
	 */
	unsigned int		subject_to_srp:1;

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

	/* Don't just dereference this pointer in kernel space!
	 * It might very well point to junk or nothing at all.
	 * NULL indicates that the task has not requested any non-preemptable
	 * section support.
	 * Not inherited upon fork.
	 */
	short* 			np_flag;

	/* For the FMLP under PSN-EDF, it is required to make the task
	 * non-preemptive from kernel space. In order not to interfere with
	 * user space, this counter indicates the kernel space np setting.
	 * kernel_np > 0 => task is non-preemptive
	 */
	unsigned int 		kernel_np;

	/* This field can be used by plugins to store where the task
	 * is currently scheduled. It is the responsibility of the
	 * plugin to avoid race conditions.
	 *
	 * Used by GSN-EDF.
	 */
	volatile int		scheduled_on;

	/* This field can be used by plugins to store where the task
	 * is currently linked. It is the responsibility of the plugin
	 * to avoid race conditions.
	 *
	 * Used by GSN-EDF.
	 */
	volatile int		linked_on;

	/* Fields saved before BE->RT transition.
	 */
	int old_policy;
	int old_prio;
};

/*	Possible RT flags	*/
#define RT_F_RUNNING		0x00000000
#define RT_F_SLEEP		0x00000001
#define RT_F_EXIT_SEM		0x00000008

#endif

#endif
