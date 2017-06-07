#include <linux/types.h>

#include <litmus/feather_trace.h>

#if !defined(CONFIG_ARCH_HAS_FEATHER_TRACE) || defined(CONFIG_RELOCATABLE)
/* provide dummy implementation */

int ft_events[MAX_EVENTS];

int ft_enable_event(unsigned long id)
{
	if (id < MAX_EVENTS) {
		ft_events[id]++;
		return 1;
	} else
		return 0;
}

int ft_disable_event(unsigned long id)
{
	if (id < MAX_EVENTS && ft_events[id]) {
		ft_events[id]--;
		return 1;
	} else
		return 0;
}

int ft_disable_all_events(void)
{
	int i;

	for (i = 0; i < MAX_EVENTS; i++)
		ft_events[i] = 0;

	return MAX_EVENTS;
}

int ft_is_event_enabled(unsigned long id)
{
	return 	id < MAX_EVENTS && ft_events[id];
}

#endif
