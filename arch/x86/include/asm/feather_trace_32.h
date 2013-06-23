/* Copyright (c) 2007-2012 Björn Brandenburg, <bbb@mpi-sws.org>
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

#define feather_callback __attribute__((regparm(3)))  __attribute__((used))

/*
 * Make the compiler reload any register that is not saved in a cdecl function
 * call (minus the registers that we explicitly clobber as output registers).
 */
#define __FT_CLOBBER_LIST0 "memory", "cc", "eax", "edx", "ecx"
#define __FT_CLOBBER_LIST1 "memory", "cc", "eax", "ecx"
#define __FT_CLOBBER_LIST2 "memory", "cc", "eax"
#define __FT_CLOBBER_LIST3 "memory", "cc", "eax"

#define __FT_TMP1(x) "=d" (x)
#define __FT_ARG1(x) "0" ((long) (x))
#define __FT_TMP2(x) "=c" (x)
#define __FT_ARG2(x) "1" ((long) (x))

#define __FT_ARG3(x) "r" ((long) (x))

#define ft_event(id, callback)                                  \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " call " #callback "                          \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
	    : : : __FT_CLOBBER_LIST0)

#define ft_event0(id, callback)                                 \
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
            " movl $" #id  ", %%eax                       \n\t" \
	    " call " #callback "                          \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
	    : : : __FT_CLOBBER_LIST0)

#define ft_event1(id, callback, param)				\
	do {							\
		long __ft_tmp1;					\
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
            " movl $" #id  ", %%eax                       \n\t" \
	    " call " #callback "                          \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
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
            " movl $" #id  ", %%eax                       \n\t" \
	    " call " #callback "                          \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
	    : __FT_TMP1(__ft_tmp1), __FT_TMP2(__ft_tmp2)	\
	    : __FT_ARG1(param), __FT_ARG2(param2)		\
	    : __FT_CLOBBER_LIST2);				\
	} while (0);


#define ft_event3(id, callback, param, param2, param3)		\
	do {							\
		long __ft_tmp1, __ft_tmp2;			\
        __asm__ __volatile__(                                   \
            "1: jmp 2f                                    \n\t" \
	    " subl $4, %%esp                              \n\t" \
            " movl $" #id  ", %%eax                       \n\t" \
	    " movl %2, (%%esp)                            \n\t" \
	    " call " #callback "                          \n\t" \
	    " addl $4, %%esp                              \n\t" \
            ".section __event_table, \"aw\"               \n\t" \
            ".long " #id  ", 0, 1b, 2f                    \n\t" \
            ".previous                                    \n\t" \
            "2:                                           \n\t" \
	    : __FT_TMP1(__ft_tmp1), __FT_TMP2(__ft_tmp2)	\
	    : __FT_ARG1(param), __FT_ARG2(param2), __FT_ARG3(param3)	\
	    : __FT_CLOBBER_LIST3);				\
	} while (0);
