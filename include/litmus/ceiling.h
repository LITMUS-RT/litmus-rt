#ifndef _LITMUS_CEILING_H_
#define _LITMUS_CEILING_H_

#ifdef CONFIG_LITMUS_LOCKING

void __srp_ceiling_block(struct task_struct *cur);

DECLARE_PER_CPU(int, srp_objects_in_use);

/* assumes preemptions off */
void srp_ceiling_block(void)
{
	struct task_struct *tsk = current;

	/* Only applies to real-time tasks. */
	if (!is_realtime(tsk))
		return;

	/* Bail out early if there aren't any SRP resources around. */
	if (likely(!raw_cpu_read(srp_objects_in_use)))
		return;

	/* Avoid recursive ceiling blocking. */
	if (unlikely(tsk->rt_param.srp_non_recurse))
		return;

	/* must take slow path */
	__srp_ceiling_block(tsk);
}

#else
#define srp_ceiling_block() /* nothing */
#endif


#endif