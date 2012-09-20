#ifndef _LITMUS_TRACE_IRQ_H_
#define	_LITMUS_TRACE_IRQ_H_

#ifdef CONFIG_SCHED_OVERHEAD_TRACE

#include <linux/percpu.h>

extern DEFINE_PER_CPU(atomic_t, irq_fired_count);

static inline void ft_irq_fired(void)
{
	/* Only called with preemptions disabled.  */
	atomic_inc(&__get_cpu_var(irq_fired_count));
}


#else

#define ft_irq_fired() /* nothing to do */

#endif

#endif
