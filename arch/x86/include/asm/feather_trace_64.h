/* Do not directly include this file. Include feather_trace.h instead */

/* regparm is the default on x86_64 */
#define feather_callback

# define _EVENT_TABLE(id,from,to) \
            ".section __event_table, \"aw\"\n\t" \
	    ".balign 8\n\t" \
            ".quad " #id  ", 0, " #from ", " #to " \n\t" \
            ".previous \n\t"

/*
 * x86_64 callee only owns rbp, rbx, r12 -> r15
 * the called can freely modify the others
 */
#define CLOBBER_LIST	"memory", "cc", "rdi", "rsi", "rdx", "rcx", \
			"r8", "r9", "r10", "r11", "rax"

#define ft_event(id, callback)                                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " call " #callback "                          \n\t" \
            _EVENT_TABLE(id,1b,2f) \
            "2:                                           \n\t" \
        : : : CLOBBER_LIST)

#define ft_event0(id, callback)                                 \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
	    _EVENT_TABLE(id,1b,2f) \
            "2:                                           \n\t" \
        : :  : CLOBBER_LIST)

#define ft_event1(id, callback, param)                          \
	__asm__ __volatile__(                                   \
	    "1: jmp 2f                                    \n\t" \
	    " movq %0, %%rsi				  \n\t"	\
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
	    _EVENT_TABLE(id,1b,2f) \
	    "2:                                           \n\t" \
	: : "r" (param)  : CLOBBER_LIST)

#define ft_event2(id, callback, param, param2)                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq %1, %%rdx				  \n\t"	\
	    " movq %0, %%rsi				  \n\t"	\
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
            _EVENT_TABLE(id,1b,2f) \
            "2:                                           \n\t" \
        : : "r" (param), "r" (param2)  : CLOBBER_LIST)

#define ft_event3(id, callback, p, p2, p3)                      \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq %2, %%rcx				  \n\t"	\
	    " movq %1, %%rdx				  \n\t"	\
	    " movq %0, %%rsi				  \n\t"	\
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
            _EVENT_TABLE(id,1b,2f) \
            "2:                                           \n\t" \
        : : "r" (p), "r" (p2), "r" (p3)  : CLOBBER_LIST)
