#ifndef _ARCH_FEATHER_TRACE_H
#define _ARCH_FEATHER_TRACE_H

static inline unsigned long long ft_timestamp(void)
{
	unsigned long long ret;
	__asm__ __volatile__("rdtsc" : "=A" (ret));
	return ret;
}

#endif
