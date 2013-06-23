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

static inline void ft_atomic_dec(int *val)
{
	atomic_sub(1, (atomic_t*) val);
}

/* Don't use rewriting implementation if kernel text pages are read-only.
 * Ftrace gets around this by using the identity mapping, but that's more
 * effort that is warrented right now for Feather-Trace.
 * Eventually, it may make sense to replace Feather-Trace with ftrace.
 */
#if defined(CONFIG_ARCH_HAS_FEATHER_TRACE) && !defined(CONFIG_DEBUG_RODATA)

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
