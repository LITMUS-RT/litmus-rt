/* Fixed-priority scheduler support.
 */

#ifndef __FP_COMMON_H__
#define __FP_COMMON_H__

#include <litmus/rt_domain.h>

#include <asm/bitops.h>


void fp_domain_init(rt_domain_t* rt, check_resched_needed_t resched,
		    release_jobs_t release);

int fp_higher_prio(struct task_struct* first,
		   struct task_struct* second);

int fp_ready_order(struct bheap_node* a, struct bheap_node* b);

#define FP_PRIO_BIT_WORDS (LITMUS_MAX_PRIORITY / BITS_PER_LONG)

#if (LITMUS_MAX_PRIORITY % BITS_PER_LONG)
#error LITMUS_MAX_PRIORITY must be a multiple of BITS_PER_LONG
#endif

/* bitmask-inexed priority queue */
struct fp_prio_queue {
	unsigned long	bitmask[FP_PRIO_BIT_WORDS];
	struct bheap	queue[LITMUS_MAX_PRIORITY];
};

void fp_prio_queue_init(struct fp_prio_queue* q);

static inline void fpq_set(struct fp_prio_queue* q, unsigned int index)
{
	unsigned long *word = q->bitmask + (index / BITS_PER_LONG);
	__set_bit(index % BITS_PER_LONG, word);
}

static inline void fpq_clear(struct fp_prio_queue* q, unsigned int index)
{
	unsigned long *word = q->bitmask + (index / BITS_PER_LONG);
	__clear_bit(index % BITS_PER_LONG, word);
}

static inline unsigned int fpq_find(struct fp_prio_queue* q)
{
	int i;

	/* loop optimizer should unroll this */
	for (i = 0; i < FP_PRIO_BIT_WORDS; i++)
		if (q->bitmask[i])
			return __ffs(q->bitmask[i]) + i * BITS_PER_LONG;

	return LITMUS_MAX_PRIORITY; /* nothing found */
}

static inline void fp_prio_add(struct fp_prio_queue* q, struct task_struct* t, unsigned int index)
{
	BUG_ON(index >= LITMUS_MAX_PRIORITY);
	BUG_ON(bheap_node_in_heap(tsk_rt(t)->heap_node));

	fpq_set(q, index);
	bheap_insert(fp_ready_order, &q->queue[index], tsk_rt(t)->heap_node);
}

static inline void fp_prio_remove(struct fp_prio_queue* q, struct task_struct* t, unsigned int index)
{
	BUG_ON(!is_queued(t));

	bheap_delete(fp_ready_order, &q->queue[index], tsk_rt(t)->heap_node);
	if (likely(bheap_empty(&q->queue[index])))
		fpq_clear(q, index);
}

static inline struct task_struct* fp_prio_peek(struct fp_prio_queue* q)
{
	unsigned int idx = fpq_find(q);
	struct bheap_node* hn;

	if (idx < LITMUS_MAX_PRIORITY) {
		hn = bheap_peek(fp_ready_order, &q->queue[idx]);
		return bheap2task(hn);
	} else
		return NULL;
}

static inline struct task_struct* fp_prio_take(struct fp_prio_queue* q)
{
	unsigned int idx = fpq_find(q);
	struct bheap_node* hn;

	if (idx < LITMUS_MAX_PRIORITY) {
		hn = bheap_take(fp_ready_order, &q->queue[idx]);
		if (likely(bheap_empty(&q->queue[idx])))
			fpq_clear(q, idx);
		return bheap2task(hn);
	} else
		return NULL;
}

int fp_preemption_needed(struct fp_prio_queue*  q, struct task_struct *t);


#endif
