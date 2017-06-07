#ifndef _ARCH_FEATHER_TRACE_H
#define _ARCH_FEATHER_TRACE_H

#include <asm/msr.h>
#include <asm/timex.h>

static inline unsigned long long ft_timestamp(void)
{
	return get_cycles();
}

#ifdef CONFIG_X86_32
#include "feather_trace_32.h"
#else
#include "feather_trace_64.h"
#endif

#endif
