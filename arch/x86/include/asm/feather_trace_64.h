/* Copyright (c) 2010 Andrea Bastoni, <bastoni@cs.unc.edu>
 * Copyright (c) 2012 Björn Brandenburg, <bbb@mpi-sws.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Do not directly include this file. Include feather_trace.h instead */

/* regparm is the default on x86_64 */
#define feather_callback  __attribute__((used))

#define __FT_EVENT_TABLE(id,from,to) \
            ".section __event_table, \"aw\"\n\t" \
	    ".balign 8\n\t" \
            ".quad " #id  ", 0, " #from ", " #to " \n\t" \
            ".previous \n\t"

/*
 * x86_64 caller only owns rbp, rbx, r12-r15;
 * the callee can freely modify the others.
 */
#define __FT_CLOBBER_LIST0	"memory", "cc", "rdi", "rsi", "rdx", "rcx", \
			"r8", "r9", "r10", "r11", "rax"

#define __FT_CLOBBER_LIST1	"memory", "cc", "rdi", "rdx", "rcx", \
			"r8", "r9", "r10", "r11", "rax"

#define __FT_CLOBBER_LIST2	"memory", "cc", "rdi", "rcx", \
			"r8", "r9", "r10", "r11", "rax"

#define __FT_CLOBBER_LIST3	"memory", "cc", "rdi", \
			"r8", "r9", "r10", "r11", "rax"

/* The registers RDI, RSI, RDX, RCX, R8 and R9 are used for integer and pointer
 * arguments. */

/* RSI */
#define __FT_TMP1(x) "=S" (x)
#define __FT_ARG1(x) "0" ((long) (x))

/* RDX */
#define __FT_TMP2(x) "=d" (x)
#define __FT_ARG2(x) "1" ((long) (x))

/* RCX */
#define __FT_TMP3(x) "=c" (x)
#define __FT_ARG3(x) "2" ((long) (x))

#define ft_event(id, callback)                                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " call " #callback "                          \n\t" \
            __FT_EVENT_TABLE(id,1b,2f)				\
            "2:                                           \n\t" \
        : : : __FT_CLOBBER_LIST0)

#define ft_event0(id, callback)                                 \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
	    __FT_EVENT_TABLE(id,1b,2f)				\
            "2:                                           \n\t" \
        : :  : __FT_CLOBBER_LIST0)

#define ft_event1(id, callback, param)                          \
	do {							\
		long __ft_tmp1;					\
	__asm__ __volatile__(                                   \
	    "1: jmp 2f                                    \n\t" \
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
	    __FT_EVENT_TABLE(id,1b,2f)				\
	    "2:                                           \n\t" \
	    : __FT_TMP1(__ft_tmp1)				\
	    : __FT_ARG1(param)					\
	    : __FT_CLOBBER_LIST1);				\
	} while (0);

#define ft_event2(id, callback, param, param2)                  \
	do {							\
		long __ft_tmp1, __ft_tmp2;			\
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
            __FT_EVENT_TABLE(id,1b,2f)				\
            "2:                                           \n\t" \
	    : __FT_TMP1(__ft_tmp1), __FT_TMP2(__ft_tmp2)	\
	    : __FT_ARG1(param), __FT_ARG2(param2)		\
	    : __FT_CLOBBER_LIST2);				\
	} while (0);

#define ft_event3(id, callback, param, param2, param3)		\
	do {							\
		long __ft_tmp1, __ft_tmp2, __ft_tmp3;		\
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " movq $" #id ", %%rdi			  \n\t" \
	    " call " #callback "                          \n\t" \
            __FT_EVENT_TABLE(id,1b,2f)				\
            "2:                                           \n\t" \
	    : __FT_TMP1(__ft_tmp1), __FT_TMP2(__ft_tmp2), __FT_TMP3(__ft_tmp3) \
	    : __FT_ARG1(param), __FT_ARG2(param2), __FT_ARG3(param3)	\
	    : __FT_CLOBBER_LIST3);				\
	} while (0);
