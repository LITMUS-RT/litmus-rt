#include "litmus/cheap.h"

static unsigned int __cheap_parent(unsigned int child)
{
	return (child - 1) / 2;
}

static unsigned int __cheap_left_child(unsigned int parent)
{
	return parent * 2 + 1;
}

static unsigned int __cheap_right_child(unsigned int parent)
{
	return parent * 2 + 2;
}

static void __cheap_swap(struct cheap_node* a, struct cheap_node* b)
{
	unsigned int	tag;
	void*		val;
	tag      = a->tag;
	val      = a->value;
	a->tag   = b->tag;
	a->value = b->value;
	b->tag   = tag;
	b->value = val;
}

void cheap_init(struct cheap* ch, unsigned int size,
		struct cheap_node* nodes)
{
	unsigned int i;
	spin_lock_init(&ch->lock);
	ch->next = 0;
	ch->size = size;
	ch->heap = nodes;

	for (i = 0; i < size; i++) {
		spin_lock_init(&ch->heap[i].lock);
		ch->heap[i].tag   = CHEAP_EMPTY;
		ch->heap[i].value = NULL;
	}
}

void* cheap_peek(struct cheap* ch)
{
	void* val;
	spin_lock(&ch->heap[CHEAP_ROOT].lock);
	val = ch->heap[CHEAP_ROOT].tag != CHEAP_EMPTY ?
		ch->heap[CHEAP_ROOT].value : NULL;
	spin_unlock(&ch->heap[CHEAP_ROOT].lock);
	return val;
}

int cheap_insert(cheap_prio_t higher_prio,
		 struct cheap* ch,
		 void* item,
		 int pid)
{
	int stop = 0;
	unsigned int child, parent, locked;
	unsigned int wait_for_parent_state;

	lockdep_off(); /* generates false positives */

	spin_lock(&ch->lock);
	if (ch->next < ch->size) {
		/* ok, node allocated */
		child = ch->next++;
		spin_lock(&ch->heap[child].lock);
		ch->heap[child].tag   = pid;
		ch->heap[child].value = item;
		spin_unlock(&ch->lock);
	} else {
		/* out of space! */
		spin_unlock(&ch->lock);
		lockdep_on();
		return -1;
	}

	spin_unlock(&ch->heap[child].lock);

	/* bubble up */
	while (!stop && child > CHEAP_ROOT) {
		parent = __cheap_parent(child);
		spin_lock(&ch->heap[parent].lock);
		spin_lock(&ch->heap[child].lock);
		locked = child;
		wait_for_parent_state = CHEAP_EMPTY;
		if (ch->heap[parent].tag == CHEAP_READY &&
		    ch->heap[child].tag == pid) {
			/* no interference */
			if (higher_prio(ch->heap[child].value,
					ch->heap[parent].value)) {
				/* out of order; swap and move up */
				__cheap_swap(ch->heap + child,
					     ch->heap + parent);
				child = parent;
			} else {
				/* In order; we are done. */
				ch->heap[child].tag = CHEAP_READY;
				stop = 1;
			}
		} else if (ch->heap[parent].tag == CHEAP_EMPTY) {
			/* Concurrent extract moved child to root;
			 * we are done.
			 */
			stop = 1;
		} else if (ch->heap[child].tag != pid) {
			/* Concurrent extract moved child up;
			 * we go after it.
			 */
			child = parent;
		} else {
			/* Some other process needs to act first.
			 * We need to back off a little in order
			 * to give the others a chance to acquire the
			 * parent's lock.
			 */
			wait_for_parent_state = ch->heap[parent].tag;
		}

		spin_unlock(&ch->heap[locked].lock);
		spin_unlock(&ch->heap[parent].lock);

	        while (wait_for_parent_state != CHEAP_EMPTY &&
		       ((volatile unsigned int) ch->heap[parent].tag) ==
		       wait_for_parent_state)
			cpu_relax();

	}
	if (!stop && child == CHEAP_ROOT) {
		spin_lock(&ch->heap[child].lock);
		if (ch->heap[child].tag == pid)
			ch->heap[child].tag = CHEAP_READY;
		spin_unlock(&ch->heap[child].lock);
	}

	lockdep_on();
	return 0;
}

void* cheap_take_if(cheap_take_predicate_t pred,
		    void* pred_ctx,
		    cheap_prio_t higher_prio,
		    struct cheap* ch)
{
	void *val, *cval;
	unsigned int ctag;
	unsigned int left, right, child, parent;

	lockdep_off();
	spin_lock(&ch->lock);
	if (ch->next > CHEAP_ROOT) {
		child = ch->next - 1;
		spin_lock(&ch->heap[child].lock);
		/* see if callback wants this item
		 */
		if (!pred || pred(pred_ctx, ch->heap[child].value))
			/* yes, proceed */
			ch->next--;
		else {
			/* no, cleanup and return */
			spin_unlock(&ch->heap[child].lock);
			child = ch->size;
		}
	} else
		child = ch->size;
	spin_unlock(&ch->lock);

	if (child == ch->size) {
		lockdep_on();
		/* empty heap */
		return NULL;
	}

	/* take value from last leaf */
	cval = ch->heap[child].value;
	ctag = ch->heap[child].tag;
	/* free last leaf */
	ch->heap[child].tag   = CHEAP_EMPTY;
	ch->heap[child].value = NULL;

	/* unlock before locking root to maintain locking order */
	spin_unlock(&ch->heap[child].lock);

	spin_lock(&ch->heap[CHEAP_ROOT].lock);
	if (ch->heap[CHEAP_ROOT].tag == CHEAP_EMPTY) {
		/* heap became empty, we got the last one */
		spin_unlock(&ch->heap[CHEAP_ROOT].lock);
		lockdep_on();
		return cval;
	} else {
		/* grab value of root (=min), replace with
		 * what we got from the last leaf
		 */
		val = ch->heap[CHEAP_ROOT].value;
		ch->heap[CHEAP_ROOT].value = cval;
		ch->heap[CHEAP_ROOT].tag   = CHEAP_READY;
	}

	/* Bubble down. We are still holding the ROOT (=parent) lock. */
	child  = 0;
	parent = CHEAP_ROOT;
	while (parent < __cheap_parent(ch->size)) {
		left  = __cheap_left_child(parent);
		right = __cheap_right_child(parent);
		spin_lock(&ch->heap[left].lock);
		if (ch->heap[left].tag == CHEAP_EMPTY) {
			/* end of the heap, done */
			spin_unlock(&ch->heap[left].lock);
			break;
		} else if (right < ch->size) {
			/* right child node exists */
			spin_lock(&ch->heap[right].lock);
			if (ch->heap[right].tag == CHEAP_EMPTY ||
			    higher_prio(ch->heap[left].value,
					ch->heap[right].value)) {
				/* left child node has higher priority */
				spin_unlock(&ch->heap[right].lock);
				child = left;
			} else {
				/* right child node has higher priority */
				spin_unlock(&ch->heap[left].lock);
				child = right;
			}
		} else {
			/* right child node does not exist */
			child = left;
		}
		if (higher_prio(ch->heap[child].value,
				ch->heap[parent].value)) {
			/* parent and child out of order */
			__cheap_swap(ch->heap + child,
				     ch->heap + parent);
			spin_unlock(&ch->heap[parent].lock);
			/* move down */
			parent = child;
		} else {
			/* in order; we are done. */
			spin_unlock(&ch->heap[child].lock);
			break;
		}
	}
	spin_unlock(&ch->heap[parent].lock);
	lockdep_on();
	return val;
}
