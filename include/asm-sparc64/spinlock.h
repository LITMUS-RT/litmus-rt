/* spinlock.h: 64-bit Sparc spinlock support.
 *
 * Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef __SPARC64_SPINLOCK_H
#define __SPARC64_SPINLOCK_H

#include <linux/threads.h>	/* For NR_CPUS */

#ifndef __ASSEMBLY__

/* To get debugging spinlocks which detect and catch
 * deadlock situations, set CONFIG_DEBUG_SPINLOCK
 * and rebuild your kernel.
 */

#define __raw_spin_is_locked(lp)	((lp)->tail != (lp)->head)

#define __raw_spin_unlock_wait(lp)	\
	do {	rmb();			\
	} while((lp)->tail != (lp)->head)



static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
	int ticket, tmp;
	__asm__ __volatile__(
"1:	lduw		[%2], %0 \n" /* read ticket */
"	add		%0, 1, %1 \n"
"	cas		[%2], %0, %1 \n"
"	cmp		%0, %1 \n"
"	be,a,pt		%%icc, 2f \n"
"	 nop \n"
"	membar		#LoadLoad | #StoreLoad | #LoadStore\n"
"	ba		1b\n"
"	 nop \n"
"2:	lduw		[%3], %1 \n"
"	cmp		%0, %1 \n"
"	be,a,pt		%%icc, 3f \n"
"	 nop \n"
"	membar		#LoadLoad | #StoreLoad | #LoadStore\n"
"	ba		2b\n"
"3:	membar		#StoreStore | #StoreLoad"
	: "=&r" (ticket), "=&r" (tmp)
	: "r" (&lock->tail), "r" (&lock->head)
	: "memory");
}

static inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
	int tail, head;
	__asm__ __volatile__(
"	lduw		[%2], %0 \n" /* read tail */
"	lduw		[%3], %1 \n" /* read head */
"	cmp		%0, %1 \n"
"	bne,a,pn	%%icc, 1f \n"
"	 nop \n"
"	inc		%1 \n"
"	cas		[%2], %0, %1 \n" /* try to inc ticket */
"	membar		#StoreStore | #StoreLoad \n"
"1:	"
	: "=&r" (tail), "=&r" (head)
	: "r" (&lock->tail), "r" (&lock->head)
	: "memory");

	return tail == head;
}

static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	int tmp;
	__asm__ __volatile__(
"	membar		#StoreStore | #LoadStore \n"
"	lduw		[%1], %0 \n"
"	inc		%0 \n"
"	st		%0, [%1] \n"
"	membar		#StoreStore | #StoreLoad"
	: "=&r" (tmp)
	: "r" (&lock->head)
	: "memory");
}

/* We don't handle this yet, but it looks like not re-enabling the interrupts
 * works fine, too. For example, lockdep also does it like this.
 */
#define __raw_spin_lock_flags(l, f) __raw_spin_lock(l)




/* Multi-reader locks, these are much saner than the 32-bit Sparc ones... */

static void inline __read_lock(raw_rwlock_t *lock)
{
	unsigned long tmp1, tmp2;

	__asm__ __volatile__ (
"1:	ldsw		[%2], %0\n"
"	brlz,pn		%0, 2f\n"
"4:	 add		%0, 1, %1\n"
"	cas		[%2], %0, %1\n"
"	cmp		%0, %1\n"
"	membar		#StoreLoad | #StoreStore\n"
"	bne,pn		%%icc, 1b\n"
"	 nop\n"
"	.subsection	2\n"
"2:	ldsw		[%2], %0\n"
"	membar		#LoadLoad\n"
"	brlz,pt		%0, 2b\n"
"	 nop\n"
"	ba,a,pt		%%xcc, 4b\n"
"	.previous"
	: "=&r" (tmp1), "=&r" (tmp2)
	: "r" (lock)
	: "memory");
}

static int inline __read_trylock(raw_rwlock_t *lock)
{
	int tmp1, tmp2;

	__asm__ __volatile__ (
"1:	ldsw		[%2], %0\n"
"	brlz,a,pn	%0, 2f\n"
"	 mov		0, %0\n"
"	add		%0, 1, %1\n"
"	cas		[%2], %0, %1\n"
"	cmp		%0, %1\n"
"	membar		#StoreLoad | #StoreStore\n"
"	bne,pn		%%icc, 1b\n"
"	 mov		1, %0\n"
"2:"
	: "=&r" (tmp1), "=&r" (tmp2)
	: "r" (lock)
	: "memory");

	return tmp1;
}

static void inline __read_unlock(raw_rwlock_t *lock)
{
	unsigned long tmp1, tmp2;

	__asm__ __volatile__(
"	membar	#StoreLoad | #LoadLoad\n"
"1:	lduw	[%2], %0\n"
"	sub	%0, 1, %1\n"
"	cas	[%2], %0, %1\n"
"	cmp	%0, %1\n"
"	bne,pn	%%xcc, 1b\n"
"	 nop"
	: "=&r" (tmp1), "=&r" (tmp2)
	: "r" (lock)
	: "memory");
}

static void inline __write_lock(raw_rwlock_t *lock)
{
	unsigned long mask, tmp1, tmp2;

	mask = 0x80000000UL;

	__asm__ __volatile__(
"1:	lduw		[%2], %0\n"
"	brnz,pn		%0, 2f\n"
"4:	 or		%0, %3, %1\n"
"	cas		[%2], %0, %1\n"
"	cmp		%0, %1\n"
"	membar		#StoreLoad | #StoreStore\n"
"	bne,pn		%%icc, 1b\n"
"	 nop\n"
"	.subsection	2\n"
"2:	lduw		[%2], %0\n"
"	membar		#LoadLoad\n"
"	brnz,pt		%0, 2b\n"
"	 nop\n"
"	ba,a,pt		%%xcc, 4b\n"
"	.previous"
	: "=&r" (tmp1), "=&r" (tmp2)
	: "r" (lock), "r" (mask)
	: "memory");
}

static void inline __write_unlock(raw_rwlock_t *lock)
{
	__asm__ __volatile__(
"	membar		#LoadStore | #StoreStore\n"
"	stw		%%g0, [%0]"
	: /* no outputs */
	: "r" (lock)
	: "memory");
}

static int inline __write_trylock(raw_rwlock_t *lock)
{
	unsigned long mask, tmp1, tmp2, result;

	mask = 0x80000000UL;

	__asm__ __volatile__(
"	mov		0, %2\n"
"1:	lduw		[%3], %0\n"
"	brnz,pn		%0, 2f\n"
"	 or		%0, %4, %1\n"
"	cas		[%3], %0, %1\n"
"	cmp		%0, %1\n"
"	membar		#StoreLoad | #StoreStore\n"
"	bne,pn		%%icc, 1b\n"
"	 nop\n"
"	mov		1, %2\n"
"2:"
	: "=&r" (tmp1), "=&r" (tmp2), "=&r" (result)
	: "r" (lock), "r" (mask)
	: "memory");

	return result;
}

#define __raw_read_lock(p)	__read_lock(p)
#define __raw_read_trylock(p)	__read_trylock(p)
#define __raw_read_unlock(p)	__read_unlock(p)
#define __raw_write_lock(p)	__write_lock(p)
#define __raw_write_unlock(p)	__write_unlock(p)
#define __raw_write_trylock(p)	__write_trylock(p)

#define __raw_read_can_lock(rw)		(!((rw)->lock & 0x80000000UL))
#define __raw_write_can_lock(rw)	(!(rw)->lock)

#define _raw_spin_relax(lock)	cpu_relax()
#define _raw_read_relax(lock)	cpu_relax()
#define _raw_write_relax(lock)	cpu_relax()

#endif /* !(__ASSEMBLY__) */

#endif /* !(__SPARC64_SPINLOCK_H) */
