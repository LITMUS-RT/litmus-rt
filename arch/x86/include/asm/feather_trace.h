#ifndef _ARCH_FEATHER_TRACE_H
#define _ARCH_FEATHER_TRACE_H

#include <asm/msr.h>

static inline unsigned long long ft_timestamp(void)
{
	return __native_read_tsc();
}

#ifdef CONFIG_X86_32
#include "feather_trace_32.h"
#else
#include "feather_trace_64.h"
#endif

#endif
