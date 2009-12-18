/* heaps.h -- Binomial Heaps
 *
 * (c) 2008, 2009 Bjoern Brandenburg
 */

#ifndef HEAP_H
#define HEAP_H

#define NOT_IN_HEAP UINT_MAX

struct heap_node {
	struct heap_node* 	parent;
	struct heap_node* 	next;
	struct heap_node* 	child;

	unsigned int 		degree;
	void*			value;
	struct heap_node**	ref;
};

struct heap {
	struct heap_node* 	head;
	/* We cache the minimum of the heap.
	 * This speeds up repeated peek operations.
	 */
	struct heap_node*	min;
};

typedef int (*heap_prio_t)(struct heap_node* a, struct heap_node* b);

void heap_init(struct heap* heap);
void heap_node_init(struct heap_node** ref_to_heap_node_ptr, void* value);

static inline int heap_node_in_heap(struct heap_node* h)
{
	return h->degree != NOT_IN_HEAP;
}

static inline int heap_empty(struct heap* heap)
{
	return heap->head == NULL && heap->min == NULL;
}

/* insert (and reinitialize) a node into the heap */
void heap_insert(heap_prio_t higher_prio,
		 struct heap* heap,
		 struct heap_node* node);

/* merge addition into target */
void heap_union(heap_prio_t higher_prio,
		struct heap* target,
		struct heap* addition);

struct heap_node* heap_peek(heap_prio_t higher_prio,
			    struct heap* heap);

struct heap_node* heap_take(heap_prio_t higher_prio,
			    struct heap* heap);

void heap_uncache_min(heap_prio_t higher_prio, struct heap* heap);
int  heap_decrease(heap_prio_t higher_prio, struct heap_node* node);

void heap_delete(heap_prio_t higher_prio,
		 struct heap* heap,
		 struct heap_node* node);

/* allocate from memcache */
struct heap_node* heap_node_alloc(int gfp_flags);
void heap_node_free(struct heap_node* hn);

/* allocate a heap node for value and insert into the heap */
int heap_add(heap_prio_t higher_prio, struct heap* heap,
	     void* value, int gfp_flags);

void* heap_take_del(heap_prio_t higher_prio,
		    struct heap* heap);
#endif
