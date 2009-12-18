#include "linux/kernel.h"
#include "litmus/heap.h"

void heap_init(struct heap* heap)
{
	heap->head = NULL;
	heap->min  = NULL;
}

void heap_node_init(struct heap_node** _h, void* value)
{
	struct heap_node* h = *_h;
	h->parent = NULL;
	h->next   = NULL;
	h->child  = NULL;
	h->degree = NOT_IN_HEAP;
	h->value  = value;
	h->ref    = _h;
}


/* make child a subtree of root */
static void __heap_link(struct heap_node* root,
			struct heap_node* child)
{
	child->parent = root;
	child->next   = root->child;
	root->child   = child;
	root->degree++;
}

/* merge root lists */
static  struct heap_node* __heap_merge(struct heap_node* a,
					     struct heap_node* b)
{
	struct heap_node* head = NULL;
	struct heap_node** pos = &head;

	while (a && b) {
		if (a->degree < b->degree) {
			*pos = a;
			a = a->next;
		} else {
			*pos = b;
			b = b->next;
		}
		pos = &(*pos)->next;
	}
	if (a)
		*pos = a;
	else
		*pos = b;
	return head;
}

/* reverse a linked list of nodes. also clears parent pointer */
static  struct heap_node* __heap_reverse(struct heap_node* h)
{
	struct heap_node* tail = NULL;
	struct heap_node* next;

	if (!h)
		return h;

	h->parent = NULL;
	while (h->next) {
		next    = h->next;
		h->next = tail;
		tail    = h;
		h       = next;
		h->parent = NULL;
	}
	h->next = tail;
	return h;
}

static  void __heap_min(heap_prio_t higher_prio, struct heap* heap,
			      struct heap_node** prev, struct heap_node** node)
{
	struct heap_node *_prev, *cur;
	*prev = NULL;

	if (!heap->head) {
		*node = NULL;
		return;
	}

	*node = heap->head;
	_prev = heap->head;
	cur   = heap->head->next;
	while (cur) {
		if (higher_prio(cur, *node)) {
			*node = cur;
			*prev = _prev;
		}
		_prev = cur;
		cur   = cur->next;
	}
}

static  void __heap_union(heap_prio_t higher_prio, struct heap* heap,
				struct heap_node* h2)
{
	struct heap_node* h1;
	struct heap_node *prev, *x, *next;
	if (!h2)
		return;
	h1 = heap->head;
	if (!h1) {
		heap->head = h2;
		return;
	}
	h1 = __heap_merge(h1, h2);
	prev = NULL;
	x    = h1;
	next = x->next;
	while (next) {
		if (x->degree != next->degree ||
		    (next->next && next->next->degree == x->degree)) {
			/* nothing to do, advance */
			prev = x;
			x    = next;
		} else if (higher_prio(x, next)) {
			/* x becomes the root of next */
			x->next = next->next;
			__heap_link(x, next);
		} else {
			/* next becomes the root of x */
			if (prev)
				prev->next = next;
			else
				h1 = next;
			__heap_link(next, x);
			x = next;
		}
		next = x->next;
	}
	heap->head = h1;
}

static struct heap_node* __heap_extract_min(heap_prio_t higher_prio,
					    struct heap* heap)
{
	struct heap_node *prev, *node;
	__heap_min(higher_prio, heap, &prev, &node);
	if (!node)
		return NULL;
	if (prev)
		prev->next = node->next;
	else
		heap->head = node->next;
	__heap_union(higher_prio, heap, __heap_reverse(node->child));
	return node;
}

/* insert (and reinitialize) a node into the heap */
void heap_insert(heap_prio_t higher_prio, struct heap* heap,
		 struct heap_node* node)
{
	struct heap_node *min;
	node->child  = NULL;
	node->parent = NULL;
	node->next   = NULL;
	node->degree = 0;
	if (heap->min && higher_prio(node, heap->min)) {
		/* swap min cache */
		min = heap->min;
		min->child  = NULL;
		min->parent = NULL;
		min->next   = NULL;
		min->degree = 0;
		__heap_union(higher_prio, heap, min);
		heap->min   = node;
	} else
		__heap_union(higher_prio, heap, node);
}

void heap_uncache_min(heap_prio_t higher_prio, struct heap* heap)
{
	struct heap_node* min;
	if (heap->min) {
		min = heap->min;
		heap->min = NULL;
		heap_insert(higher_prio, heap, min);
	}
}

/* merge addition into target */
void heap_union(heap_prio_t higher_prio,
		struct heap* target, struct heap* addition)
{
	/* first insert any cached minima, if necessary */
	heap_uncache_min(higher_prio, target);
	heap_uncache_min(higher_prio, addition);
	__heap_union(higher_prio, target, addition->head);
	/* this is a destructive merge */
	addition->head = NULL;
}

struct heap_node* heap_peek(heap_prio_t higher_prio,
			    struct heap* heap)
{
	if (!heap->min)
		heap->min = __heap_extract_min(higher_prio, heap);
	return heap->min;
}

struct heap_node* heap_take(heap_prio_t higher_prio,
			    struct heap* heap)
{
	struct heap_node *node;
	if (!heap->min)
		heap->min = __heap_extract_min(higher_prio, heap);
	node = heap->min;
	heap->min = NULL;
	if (node)
		node->degree = NOT_IN_HEAP;
	return node;
}

int heap_decrease(heap_prio_t higher_prio, struct heap_node* node)
{
	struct heap_node  *parent;
	struct heap_node** tmp_ref;
	void* tmp;

	/* bubble up */
	parent = node->parent;
	while (parent && higher_prio(node, parent)) {
		/* swap parent and node */
		tmp           = parent->value;
		parent->value = node->value;
		node->value   = tmp;
		/* swap references */
		*(parent->ref) = node;
		*(node->ref)   = parent;
		tmp_ref        = parent->ref;
		parent->ref    = node->ref;
		node->ref      = tmp_ref;
		/* step up */
		node   = parent;
		parent = node->parent;
	}

	return parent != NULL;
}

void heap_delete(heap_prio_t higher_prio, struct heap* heap,
		 struct heap_node* node)
{
	struct heap_node *parent, *prev, *pos;
	struct heap_node** tmp_ref;
	void* tmp;

	if (heap->min != node) {
		/* bubble up */
		parent = node->parent;
		while (parent) {
			/* swap parent and node */
			tmp           = parent->value;
			parent->value = node->value;
			node->value   = tmp;
			/* swap references */
			*(parent->ref) = node;
			*(node->ref)   = parent;
			tmp_ref        = parent->ref;
			parent->ref    = node->ref;
			node->ref      = tmp_ref;
			/* step up */
			node   = parent;
			parent = node->parent;
		}
		/* now delete:
		 * first find prev */
		prev = NULL;
		pos  = heap->head;
		while (pos != node) {
			prev = pos;
			pos  = pos->next;
		}
		/* we have prev, now remove node */
		if (prev)
			prev->next = node->next;
		else
			heap->head = node->next;
		__heap_union(higher_prio, heap, __heap_reverse(node->child));
	} else
		heap->min = NULL;
	node->degree = NOT_IN_HEAP;
}

/* allocate a heap node for value and insert into the heap */
int heap_add(heap_prio_t higher_prio, struct heap* heap,
	     void* value, int gfp_flags)
{
	struct heap_node* hn = heap_node_alloc(gfp_flags);
	if (likely(hn)) {
		heap_node_init(&hn, value);
		heap_insert(higher_prio, heap, hn);
	}
	return hn != NULL;
}

void* heap_take_del(heap_prio_t higher_prio,
		    struct heap* heap)
{
	struct heap_node* hn = heap_take(higher_prio, heap);
	void* ret = NULL;
	if (hn) {
		ret = hn->value;
		heap_node_free(hn);
	}
	return ret;
}
