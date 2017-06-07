#ifndef _FEATHER_TRACE_H_
#define _FEATHER_TRACE_H_

#include <linux/atomic.h>

int ft_enable_event(unsigned long id);
int ft_disable_event(unsigned long id);
int ft_is_event_enabled(unsigned long id);
int ft_disable_all_events(void);

/* Don't use rewriting implementation if the kernel is relocatable.
 */
#if defined(CONFIG_ARCH_HAS_FEATHER_TRACE) && !defined(CONFIG_RELOCATABLE)

#include <asm/feather_trace.h>

#else /* !__ARCH_HAS_FEATHER_TRACE */

/* provide default implementation */
#include <linux/timex.h> /* for get_cycles() */

static inline unsigned long long ft_timestamp(void)
{
	return get_cycles();
}

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
