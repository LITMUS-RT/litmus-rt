#include <linux/bug.h>
#include <linux/kernel.h>
#include <litmus/bheap.h>

void bheap_init(struct bheap* heap)
{
	heap->head = NULL;
	heap->min  = NULL;
}

void bheap_node_init(struct bheap_node** _h, void* value)
{
	struct bheap_node* h = *_h;
	h->parent = NULL;
	h->next   = NULL;
	h->child  = NULL;
	h->degree = NOT_IN_HEAP;
	h->value  = value;
	h->ref    = _h;
}


/* make child a subtree of root */
static void __bheap_link(struct bheap_node* root,
			struct bheap_node* child)
{
	child->parent = root;
	child->next   = root->child;
	root->child   = child;
	root->degree++;
}

/* merge root lists */
static  struct bheap_node* __bheap_merge(struct bheap_node* a,
					     struct bheap_node* b)
{
	struct bheap_node* head = NULL;
	struct bheap_node** pos = &head;

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
static  struct bheap_node* __bheap_reverse(struct bheap_node* h)
{
	struct bheap_node* tail = NULL;
	struct bheap_node* next;

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

static  void __bheap_min(bheap_prio_t higher_prio, struct bheap* heap,
			      struct bheap_node** prev, struct bheap_node** node)
{
	struct bheap_node *_prev, *cur;
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

static  void __bheap_union(bheap_prio_t higher_prio, struct bheap* heap,
				struct bheap_node* h2)
{
	struct bheap_node* h1;
	struct bheap_node *prev, *x, *next;
	if (!h2)
		return;
	h1 = heap->head;
	if (!h1) {
		heap->head = h2;
		return;
	}
	h1 = __bheap_merge(h1, h2);
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
			__bheap_link(x, next);
		} else {
			/* next becomes the root of x */
			if (prev)
				prev->next = next;
			else
				h1 = next;
			__bheap_link(next, x);
			x = next;
		}
		next = x->next;
	}
	heap->head = h1;
}

static struct bheap_node* __bheap_extract_min(bheap_prio_t higher_prio,
					    struct bheap* heap)
{
	struct bheap_node *prev, *node;
	__bheap_min(higher_prio, heap, &prev, &node);
	if (!node)
		return NULL;
	if (prev)
		prev->next = node->next;
	else
		heap->head = node->next;
	__bheap_union(higher_prio, heap, __bheap_reverse(node->child));
	return node;
}

/* insert (and reinitialize) a node into the heap */
void bheap_insert(bheap_prio_t higher_prio, struct bheap* heap,
		 struct bheap_node* node)
{
	struct bheap_node *min;
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
		__bheap_union(higher_prio, heap, min);
		heap->min   = node;
	} else
		__bheap_union(higher_prio, heap, node);
}

void bheap_uncache_min(bheap_prio_t higher_prio, struct bheap* heap)
{
	struct bheap_node* min;
	if (heap->min) {
		min = heap->min;
		heap->min = NULL;
		bheap_insert(higher_prio, heap, min);
	}
}

/* merge addition into target */
void bheap_union(bheap_prio_t higher_prio,
		struct bheap* target, struct bheap* addition)
{
	/* first insert any cached minima, if necessary */
	bheap_uncache_min(higher_prio, target);
	bheap_uncache_min(higher_prio, addition);
	__bheap_union(higher_prio, target, addition->head);
	/* this is a destructive merge */
	addition->head = NULL;
}

struct bheap_node* bheap_peek(bheap_prio_t higher_prio,
			    struct bheap* heap)
{
	if (!heap->min)
		heap->min = __bheap_extract_min(higher_prio, heap);
	return heap->min;
}

struct bheap_node* bheap_take(bheap_prio_t higher_prio,
			    struct bheap* heap)
{
	struct bheap_node *node;
	if (!heap->min)
		heap->min = __bheap_extract_min(higher_prio, heap);
	node = heap->min;
	heap->min = NULL;
	if (node)
		node->degree = NOT_IN_HEAP;
	return node;
}

int bheap_decrease(bheap_prio_t higher_prio, struct bheap_node* node)
{
	struct bheap_node  *parent;
	struct bheap_node** tmp_ref;
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

void bheap_delete(bheap_prio_t higher_prio, struct bheap* heap,
		 struct bheap_node* node)
{
	struct bheap_node *parent, *prev, *pos;
	struct bheap_node** tmp_ref;
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
			BUG_ON(!pos); /* fell off the list -> deleted from wrong heap */
			prev = pos;
			pos  = pos->next;
		}
		/* we have prev, now remove node */
		if (prev)
			prev->next = node->next;
		else
			heap->head = node->next;
		__bheap_union(higher_prio, heap, __bheap_reverse(node->child));
	} else
		heap->min = NULL;
	node->degree = NOT_IN_HEAP;
}

/* allocate a heap node for value and insert into the heap */
int bheap_add(bheap_prio_t higher_prio, struct bheap* heap,
	     void* value, int gfp_flags)
{
	struct bheap_node* hn = bheap_node_alloc(gfp_flags);
	if (likely(hn)) {
		bheap_node_init(&hn, value);
		bheap_insert(higher_prio, heap, hn);
	}
	return hn != NULL;
}

void* bheap_take_del(bheap_prio_t higher_prio,
		    struct bheap* heap)
{
	struct bheap_node* hn = bheap_take(higher_prio, heap);
	void* ret = NULL;
	if (hn) {
		ret = hn->value;
		bheap_node_free(hn);
	}
	return ret;
}
