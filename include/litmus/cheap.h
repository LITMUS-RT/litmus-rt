/* cheap.h -- A concurrent heap implementation.
 *
 * (c) 2009 Bjoern B. Brandenburg, <bbb@cs.unc.edu>
 *
 * Based on:
 *
 *   G. Hunt, M. Micheal, S. Parthasarath, and M. Scott
 *   "An efficient algorithm for concurrent priority queue heaps."
 *   Information Processing Letters, 60(3): 151-157, November 1996
 */

#ifndef CHEAP_H

#include "linux/spinlock.h"

#define CHEAP_EMPTY 0xffffffff
#define CHEAP_READY 0xffffff00
#define CHEAP_ROOT  0

struct cheap_node {
	spinlock_t		lock;

	unsigned int		tag;
	void*			value;
};

struct cheap {
	spinlock_t		lock;

	unsigned int		next;
	unsigned int		size;

	struct cheap_node*	heap;
};

typedef int (*cheap_prio_t)(void* a, void* b);

void cheap_init(struct cheap* ch,
		unsigned int size,
		struct cheap_node* nodes);

int cheap_insert(cheap_prio_t higher_prio,
		 struct cheap* ch,
		 void* item,
		 int pid);

void* cheap_peek(struct cheap* ch);

typedef int (*cheap_take_predicate_t)(void* ctx, void* value);

void* cheap_take_if(cheap_take_predicate_t pred,
		    void* pred_ctx,
		    cheap_prio_t higher_prio,
		    struct cheap* ch);

static inline void* cheap_take(cheap_prio_t higher_prio,
			       struct cheap* ch)
{
	return cheap_take_if(NULL, NULL, higher_prio, ch);
}

#endif
