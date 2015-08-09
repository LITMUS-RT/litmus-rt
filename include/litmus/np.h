#ifndef _LITMUS_NP_H_
#define _LITMUS_NP_H_

/* Definitions related to non-preemptive sections signaled via the control
 * page
 */

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

#endif

