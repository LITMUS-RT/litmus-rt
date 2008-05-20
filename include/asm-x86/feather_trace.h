#ifndef _ARCH_FEATHER_TRACE_H
#define _ARCH_FEATHER_TRACE_H

static inline int  fetch_and_inc(int *val)
{
	int ret = 1;
	__asm__ __volatile__("lock; xaddl %0, %1" : "+r" (ret), "+m" (*val) : : "memory" );
	return ret;
}

static inline int  fetch_and_dec(int *val)
{
	int ret = -1;
	__asm__ __volatile__("lock; xaddl %0, %1" : "+r" (ret), "+m" (*val) : : "memory" );
	return ret;
}

#define feather_callback __attribute__((regparm(0)))

/* make the compiler reload any register that is not saved in
 * a cdecl function call
 */
#define CLOBBER_LIST "memory", "cc", "eax", "ecx", "edx"

#define ft_event(id, callback)                                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " call " #callback "                          \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
        : : : CLOBBER_LIST)

#define ft_event0(id, callback)                                 \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " subl $4, %%esp                              \n\t" \
            " movl $" #id  ", (%%esp)                     \n\t" \
	    " call " #callback "                          \n\t" \
	    " addl $4, %%esp                              \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
        : :  : CLOBBER_LIST)

#define ft_event1(id, callback, param)                          \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " subl $8, %%esp                              \n\t" \
	    " movl %0, 4(%%esp)                           \n\t" \
            " movl $" #id  ", (%%esp)                     \n\t" \
	    " call " #callback "                          \n\t" \
	    " addl $8, %%esp                              \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
        : : "r" (param)  : CLOBBER_LIST)

#define ft_event2(id, callback, param, param2)                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " subl $12, %%esp                             \n\t" \
	    " movl %1, 8(%%esp)                           \n\t" \
	    " movl %0, 4(%%esp)                           \n\t" \
            " movl $" #id  ", (%%esp)                     \n\t" \
	    " call " #callback "                          \n\t" \
	    " addl $12, %%esp                             \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
        : : "r" (param), "r" (param2)  : CLOBBER_LIST)


#define ft_event3(id, callback, p, p2, p3)                      \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " subl $16, %%esp                             \n\t" \
	    " movl %1, 12(%%esp)                          \n\t" \
	    " movl %1, 8(%%esp)                           \n\t" \
	    " movl %0, 4(%%esp)                           \n\t" \
            " movl $" #id  ", (%%esp)                     \n\t" \
	    " call " #callback "                          \n\t" \
	    " addl $16, %%esp                             \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
        : : "r" (p), "r" (p2), "r" (p3)  : CLOBBER_LIST)


static inline unsigned long long ft_timestamp(void)
{
	unsigned long long ret;
	__asm__ __volatile__("rdtsc" : "=A" (ret));
	return ret;
}

#define __ARCH_HAS_FEATHER_TRACE

#endif
