#include <litmus/binheap.h>

/* Returns true of the root ancestor of node is the root of the given heap. */
int binheap_is_in_this_heap(struct binheap_node *node,
	struct binheap* heap)
{
	if(!binheap_is_in_heap(node)) {
		return 0;
	}

	while(node->parent != NULL) {
		node = node->parent;
	}

	return (node == heap->root);
}


/* Update the node reference pointers.  Same logic as Litmus binomial heap. */
static void __update_ref(struct binheap_node *parent,
				struct binheap_node *child)
{
	*(parent->ref_ptr) = child;
	*(child->ref_ptr) = parent;

	swap(parent->ref_ptr, child->ref_ptr);
}


/* Swaps data between two nodes. */
static void __binheap_swap(struct binheap_node *parent,
				struct binheap_node *child)
{
	swap(parent->data, child->data);
	__update_ref(parent, child);
}


/* Swaps memory and data between two nodes. Actual nodes swap instead of
 * just data.  Needed when we delete nodes from the heap.
 */
static void __binheap_swap_safe(struct binheap *handle,
				struct binheap_node *a,
				struct binheap_node *b)
{
	swap(a->data, b->data);
	__update_ref(a, b);

	if((a->parent != NULL) && (a->parent == b->parent)) {
		/* special case: shared parent */
		swap(a->parent->left, a->parent->right);
	}
	else {
		/* Update pointers to swap parents. */

		if(a->parent) {
			if(a == a->parent->left) {
				a->parent->left = b;
			}
			else {
				a->parent->right = b;
			}
		}

		if(b->parent) {
			if(b == b->parent->left) {
				b->parent->left = a;
			}
			else {
				b->parent->right = a;
			}
		}

		swap(a->parent, b->parent);
	}

	/* swap children */

	if(a->left) {
		a->left->parent = b;

		if(a->right) {
			a->right->parent = b;
		}
	}

	if(b->left) {
		b->left->parent = a;

		if(b->right) {
			b->right->parent = a;
		}
	}

	swap(a->left, b->left);
	swap(a->right, b->right);


	/* update next/last/root pointers */

	if(a == handle->next) {
		handle->next = b;
	}
	else if(b == handle->next) {
		handle->next = a;
	}

	if(a == handle->last) {
		handle->last = b;
	}
	else if(b == handle->last) {
		handle->last = a;
	}

	if(a == handle->root) {
		handle->root = b;
	}
	else if(b == handle->root) {
		handle->root = a;
	}
}


/**
 * Update the pointer to the last node in the complete binary tree.
 * Called internally after the root node has been deleted.
 */
static void __binheap_update_last(struct binheap *handle)
{
	struct binheap_node *temp = handle->last;

	/* find a "bend" in the tree. */
	while(temp->parent && (temp == temp->parent->left)) {
		temp = temp->parent;
	}

	/* step over to sibling if we're not at root */
	if(temp->parent != NULL) {
		temp = temp->parent->left;
	}

	/* now travel right as far as possible. */
	while(temp->right != NULL) {
		temp = temp->right;
	}

	/* take one step to the left if we're not at the bottom-most level. */
	if(temp->left != NULL) {
		temp = temp->left;
	}

	handle->last = temp;
}


/**
 * Update the pointer to the node that will take the next inserted node.
 * Called internally after a node has been inserted.
 */
static void __binheap_update_next(struct binheap *handle)
{
	struct binheap_node *temp = handle->next;

	/* find a "bend" in the tree. */
	while(temp->parent && (temp == temp->parent->right)) {
		temp = temp->parent;
	}

	/* step over to sibling if we're not at root */
	if(temp->parent != NULL) {
		temp = temp->parent->right;
	}

	/* now travel left as far as possible. */
	while(temp->left != NULL) {
		temp = temp->left;
	}

	handle->next = temp;
}



/* bubble node up towards root */
static void __binheap_bubble_up(struct binheap *handle,
				struct binheap_node *node)
{
	/* let BINHEAP_POISON data bubble to the top */

	while((node->parent != NULL) &&
		  ((node->data == BINHEAP_POISON) ||
		   handle->compare(node, node->parent))) {
			  __binheap_swap(node->parent, node);
			  node = node->parent;
	}
}


/* bubble node down, swapping with min-child */
static void __binheap_bubble_down(struct binheap *handle)
{
	struct binheap_node *node = handle->root;

	while(node->left != NULL) {
		if(node->right && handle->compare(node->right, node->left)) {
			if(handle->compare(node->right, node)) {
				__binheap_swap(node, node->right);
				node = node->right;
			}
			else {
				break;
			}
		}
		else {
			if(handle->compare(node->left, node)) {
				__binheap_swap(node, node->left);
				node = node->left;
			}
			else {
				break;
			}
		}
	}
}


void __binheap_add(struct binheap_node *new_node,
				struct binheap *handle,
				void *data)
{
	new_node->data = data;
	new_node->ref = new_node;
	new_node->ref_ptr = &(new_node->ref);

	if(!binheap_empty(handle)) {
		/* insert left side first */
		if(handle->next->left == NULL) {
			handle->next->left = new_node;
			new_node->parent = handle->next;
			new_node->left = NULL;
			new_node->right = NULL;

			handle->last = new_node;

			__binheap_bubble_up(handle, new_node);
		}
		else {
			/* left occupied. insert right. */
			handle->next->right = new_node;
			new_node->parent = handle->next;
			new_node->left = NULL;
			new_node->right = NULL;

			handle->last = new_node;

			__binheap_update_next(handle);
			__binheap_bubble_up(handle, new_node);
		}
	}
	else {
		/* first node in heap */

		new_node->parent = NULL;
		new_node->left = NULL;
		new_node->right = NULL;

		handle->root = new_node;
		handle->next = new_node;
		handle->last = new_node;
	}
}


/**
 * Removes the root node from the heap. The node is removed after coalescing
 * the binheap_node with its original data pointer at the root of the tree.
 *
 * The 'last' node in the tree is then swapped up to the root and bubbled
 * down.
 */
void __binheap_delete_root(struct binheap *handle,
				struct binheap_node *container)
{
	struct binheap_node *root = handle->root;

	if(root != container) {
		/* coalesce */
		__binheap_swap_safe(handle, root, container);
		root = container;
	}

	if(handle->last != root) {
		/* swap 'last' node up to root and bubble it down. */

		struct binheap_node *to_move = handle->last;

		if(to_move->parent != root) {
			handle->next = to_move->parent;

			if(handle->next->right == to_move) {
				/* disconnect from parent */
				to_move->parent->right = NULL;
				handle->last = handle->next->left;
			}
			else {
				/* find new 'last' before we disconnect */
				__binheap_update_last(handle);

				/* disconnect from parent */
				to_move->parent->left = NULL;
			}
		}
		else {
			/* 'last' is direct child of root */

			handle->next = to_move;

			if(to_move == to_move->parent->right) {
				to_move->parent->right = NULL;
				handle->last = to_move->parent->left;
			}
			else {
				to_move->parent->left = NULL;
				handle->last = to_move;
			}
		}
		to_move->parent = NULL;

		/* reconnect as root.  We can't just swap data ptrs since root node
		 * may be freed after this function returns.
		 */
		to_move->left = root->left;
		to_move->right = root->right;
		if(to_move->left != NULL) {
			to_move->left->parent = to_move;
		}
		if(to_move->right != NULL) {
			to_move->right->parent = to_move;
		}

		handle->root = to_move;

		/* bubble down */
		__binheap_bubble_down(handle);
	}
	else {
		/* removing last node in tree */
		handle->root = NULL;
		handle->next = NULL;
		handle->last = NULL;
	}

	/* mark as removed */
	container->parent = BINHEAP_POISON;
}


/**
 * Delete an arbitrary node.  Bubble node to delete up to the root,
 * and then delete to root.
 */
void __binheap_delete(struct binheap_node *node_to_delete,
				struct binheap *handle)
{
	struct binheap_node *target = node_to_delete->ref;
	void *temp_data = target->data;

	/* temporarily set data to null to allow node to bubble up to the top. */
	target->data = BINHEAP_POISON;

	__binheap_bubble_up(handle, target);
	__binheap_delete_root(handle, node_to_delete);

	node_to_delete->data = temp_data;  /* restore node data pointer */
}


/**
 * Bubble up a node whose pointer has decreased in value.
 */
void __binheap_decrease(struct binheap_node *orig_node,
				struct binheap *handle)
{
	struct binheap_node *target = orig_node->ref;

	__binheap_bubble_up(handle, target);
}
