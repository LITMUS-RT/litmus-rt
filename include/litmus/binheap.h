#ifndef LITMUS_BINARY_HEAP_H
#define LITMUS_BINARY_HEAP_H

#include <linux/kernel.h>

/**
 * Simple binary heap with add, arbitrary delete, delete_root, and top
 * operations.
 *
 * Style meant to conform with list.h.
 *
 * Motivation: Linux's prio_heap.h is of fixed size. Litmus's binomial
 * heap may be overkill (and perhaps not general enough) for some applications.
 *
 * Note: In order to make node swaps fast, a node inserted with a data pointer
 * may not always hold said data pointer. This is similar to the binomial heap
 * implementation. This does make node deletion tricky since we have to
 * (1) locate the node that holds the data pointer to delete, and (2) the
 * node that was originally inserted with said data pointer. These have to be
 * coalesced into a single node before removal (see usage of
 * __binheap_safe_swap()). We have to track node references to accomplish this.
 */

struct binheap_node {
	void	*data;
	struct binheap_node *parent;
	struct binheap_node *left;
	struct binheap_node *right;

	/* pointer to binheap_node that holds *data for which this binheap_node
	 * was originally inserted.  (*data "owns" this node)
	 */
	struct binheap_node *ref;
	struct binheap_node **ref_ptr;
};

/**
 * Signature of compator function.  Assumed 'less-than' (min-heap).
 * Pass in 'greater-than' for max-heap.
 *
 * TODO: Consider macro-based implementation that allows comparator to be
 * inlined (similar to Linux red/black tree) for greater efficiency.
 */
typedef int (*binheap_order_t)(struct binheap_node *a,
				struct binheap_node *b);


struct binheap {
	struct binheap_node *root;

	/* pointer to node to take next inserted child */
	struct binheap_node *next;

	/* pointer to last node in complete binary tree */
	struct binheap_node *last;

	/* comparator function pointer */
	binheap_order_t compare;
};


/* Initialized heap nodes not in a heap have parent
 * set to BINHEAP_POISON.
 */
#define BINHEAP_POISON	((void*)(0xdeadbeef))


/**
 * binheap_entry - get the struct for this heap node.
 *  Only valid when called upon heap nodes other than the root handle.
 * @ptr:	the heap node.
 * @type:	the type of struct pointed to by binheap_node::data.
 * @member:	unused.
 */
#define binheap_entry(ptr, type, member) \
((type *)((ptr)->data))

/**
 * binheap_node_container - get the struct that contains this node.
 *  Only valid when called upon heap nodes other than the root handle.
 * @ptr:	the heap node.
 * @type:	the type of struct the node is embedded in.
 * @member:	the name of the binheap_struct within the (type) struct.
 */
#define binheap_node_container(ptr, type, member) \
container_of((ptr), type, member)

/**
 * binheap_top_entry - get the struct for the node at the top of the heap.
 *  Only valid when called upon the heap handle node.
 * @ptr:    the special heap-handle node.
 * @type:   the type of the struct the head is embedded in.
 * @member:	the name of the binheap_struct within the (type) struct.
 */
#define binheap_top_entry(ptr, type, member) \
binheap_entry((ptr)->root, type, member)

/**
 * binheap_delete_root - remove the root element from the heap.
 * @handle:	 handle to the heap.
 * @type:    the type of the struct the head is embedded in.
 * @member:	 the name of the binheap_struct within the (type) struct.
 */
#define binheap_delete_root(handle, type, member) \
__binheap_delete_root((handle), &((type *)((handle)->root->data))->member)

/**
 * binheap_delete - remove an arbitrary element from the heap.
 * @to_delete:  pointer to node to be removed.
 * @handle:	 handle to the heap.
 */
#define binheap_delete(to_delete, handle) \
__binheap_delete((to_delete), (handle))

/**
 * binheap_add - insert an element to the heap
 * new_node: node to add.
 * @handle:	 handle to the heap.
 * @type:    the type of the struct the head is embedded in.
 * @member:	 the name of the binheap_struct within the (type) struct.
 */
#define binheap_add(new_node, handle, type, member) \
__binheap_add((new_node), (handle), container_of((new_node), type, member))

/**
 * binheap_decrease - re-eval the position of a node (based upon its
 * original data pointer).
 * @handle: handle to the heap.
 * @orig_node: node that was associated with the data pointer
 *             (whose value has changed) when said pointer was
 *             added to the heap.
 */
#define binheap_decrease(orig_node, handle) \
__binheap_decrease((orig_node), (handle))

#define BINHEAP_NODE_INIT() { NULL, BINHEAP_POISON, NULL, NULL , NULL, NULL}

#define BINHEAP_NODE(name) \
	struct binheap_node name = BINHEAP_NODE_INIT()


static inline void INIT_BINHEAP_NODE(struct binheap_node *n)
{
	n->data = NULL;
	n->parent = BINHEAP_POISON;
	n->left = NULL;
	n->right = NULL;
	n->ref = NULL;
	n->ref_ptr = NULL;
}

static inline void INIT_BINHEAP_HANDLE(struct binheap *handle,
				binheap_order_t compare)
{
	handle->root = NULL;
	handle->next = NULL;
	handle->last = NULL;
	handle->compare = compare;
}

/* Returns true if binheap is empty. */
static inline int binheap_empty(struct binheap *handle)
{
	return(handle->root == NULL);
}

/* Returns true if binheap node is in a heap. */
static inline int binheap_is_in_heap(struct binheap_node *node)
{
	return (node->parent != BINHEAP_POISON);
}

/* Returns true if binheap node is in given heap. */
int binheap_is_in_this_heap(struct binheap_node *node, struct binheap* heap);

/* Add a node to a heap */
void __binheap_add(struct binheap_node *new_node,
				struct binheap *handle,
				void *data);

/**
 * Removes the root node from the heap. The node is removed after coalescing
 * the binheap_node with its original data pointer at the root of the tree.
 *
 * The 'last' node in the tree is then swapped up to the root and bubbled
 * down.
 */
void __binheap_delete_root(struct binheap *handle,
				struct binheap_node *container);

/**
 * Delete an arbitrary node.  Bubble node to delete up to the root,
 * and then delete to root.
 */
void __binheap_delete(struct binheap_node *node_to_delete,
				struct binheap *handle);

/**
 * Bubble up a node whose pointer has decreased in value.
 */
void __binheap_decrease(struct binheap_node *orig_node,
						struct binheap *handle);


#endif
