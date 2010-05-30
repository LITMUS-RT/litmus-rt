#ifndef _FEATHER_TRACE_H_
#define _FEATHER_TRACE_H_

#include <asm/atomic.h>

int ft_enable_event(unsigned long id);
int ft_disable_event(unsigned long id);
int ft_is_event_enabled(unsigned long id);
int ft_disable_all_events(void);

/* atomic_* funcitons are inline anyway */
static inline int fetch_and_inc(int *val)
{
	return atomic_add_return(1, (atomic_t*) val) - 1;
}

static inline int fetch_and_dec(int *val)
{
	return atomic_sub_return(1, (atomic_t*) val) + 1;
}

#ifdef CONFIG_ARCH_HAS_FEATHER_TRACE

#include <asm/feather_trace.h>

#else /* !__ARCH_HAS_FEATHER_TRACE */

/* provide default implementation */

#define feather_callback

#define MAX_EVENTS 1024

extern int ft_events[MAX_EVENTS];

#define ft_event(id, callback) \
	if (ft_events[id]) callback();

#define ft_event0(id, callback) \
	if (ft_events[id]) callback(id);

#define ft_event1(id, callback, param) \
	if (ft_events[id]) callback(id, param);

#define ft_event2(id, callback, param, param2) \
	if (ft_events[id]) callback(id, param, param2);

#define ft_event3(id, callback, p, p2, p3) \
	if (ft_events[id]) callback(id, p, p2, p3);

#endif /* __ARCH_HAS_FEATHER_TRACE */

#endif
