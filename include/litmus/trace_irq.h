#ifndef _LITMUS_TRACE_IRQ_H_
#define	_LITMUS_TRACE_IRQ_H_

#ifdef CONFIG_SCHED_OVERHEAD_TRACE

void ft_irq_fired(void);

#else

#define ft_irq_fired() /* nothing to do */

#endif

#endif
