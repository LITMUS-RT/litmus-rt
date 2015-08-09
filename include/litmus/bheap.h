/* bheaps.h -- Binomial Heaps
 *
 * (c) 2008, 2009 Bjoern Brandenburg
 */

#ifndef BHEAP_H
#define BHEAP_H

#define NOT_IN_HEAP UINT_MAX

struct bheap_node {
	struct bheap_node* 	parent;
	struct bheap_node* 	next;
	struct bheap_node* 	child;

	unsigned int 		degree;
	void*			value;
	struct bheap_node**	ref;
};

struct bheap {
	struct bheap_node* 	head;
	/* We cache the minimum of the heap.
	 * This speeds up repeated peek operations.
	 */
	struct bheap_node*	min;
};

typedef int (*bheap_prio_t)(struct bheap_node* a, struct bheap_node* b);

void bheap_init(struct bheap* heap);
void bheap_node_init(struct bheap_node** ref_to_bheap_node_ptr, void* value);

static inline int bheap_node_in_heap(struct bheap_node* h)
{
	return h->degree != NOT_IN_HEAP;
}

static inline int bheap_empty(struct bheap* heap)
{
	return heap->head == NULL && heap->min == NULL;
}

/* insert (and reinitialize) a node into the heap */
void bheap_insert(bheap_prio_t higher_prio,
		 struct bheap* heap,
		 struct bheap_node* node);

/* merge addition into target */
void bheap_union(bheap_prio_t higher_prio,
		struct bheap* target,
		struct bheap* addition);

struct bheap_node* bheap_peek(bheap_prio_t higher_prio,
			    struct bheap* heap);

struct bheap_node* bheap_take(bheap_prio_t higher_prio,
			    struct bheap* heap);

void bheap_uncache_min(bheap_prio_t higher_prio, struct bheap* heap);
int  bheap_decrease(bheap_prio_t higher_prio, struct bheap_node* node);

void bheap_delete(bheap_prio_t higher_prio,
		 struct bheap* heap,
		 struct bheap_node* node);

/* allocate from memcache */
struct bheap_node* bheap_node_alloc(int gfp_flags);
void bheap_node_free(struct bheap_node* hn);

/* allocate a heap node for value and insert into the heap */
int bheap_add(bheap_prio_t higher_prio, struct bheap* heap,
	     void* value, int gfp_flags);

void* bheap_take_del(bheap_prio_t higher_prio,
		    struct bheap* heap);
#endif
