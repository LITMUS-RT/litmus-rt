#ifndef _ARCH_FEATHER_TRACE_H
#define _ARCH_FEATHER_TRACE_H

#include <asm/atomic.h>
#include <asm/timex.h>

static inline int  fetch_and_inc(int *val)
{
	return atomic_add_ret(1, (atomic_t*) val) - 1;
}

static inline int  fetch_and_dec(int *val)
{
	return atomic_sub_ret(1, (atomic_t*) val) + 1;
}

static inline unsigned long long ft_timestamp(void)
{
	return get_cycles();
}

#endif
