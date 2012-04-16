#ifndef _LITMUS_WAIT_H_
#define _LITMUS_WAIT_H_

struct task_struct* __waitqueue_remove_first(wait_queue_head_t *wq);

/* wrap regular wait_queue_t head */
struct __prio_wait_queue {
	wait_queue_t wq;

	/* some priority point */
	lt_t priority;
	/* break ties in priority by lower tie_breaker */
	unsigned int tie_breaker;
};

typedef struct __prio_wait_queue prio_wait_queue_t;

static inline void init_prio_waitqueue_entry(prio_wait_queue_t *pwq,
					     struct task_struct* t,
					     lt_t priority)
{
	init_waitqueue_entry(&pwq->wq, t);
	pwq->priority    = priority;
	pwq->tie_breaker = 0;
}

static inline void init_prio_waitqueue_entry_tie(prio_wait_queue_t *pwq,
						 struct task_struct* t,
						 lt_t priority,
						 unsigned int tie_breaker)
{
	init_waitqueue_entry(&pwq->wq, t);
	pwq->priority    = priority;
	pwq->tie_breaker = tie_breaker;
}

unsigned int __add_wait_queue_prio_exclusive(
	wait_queue_head_t* head,
	prio_wait_queue_t *new);

static inline unsigned int add_wait_queue_prio_exclusive(
	wait_queue_head_t* head,
	prio_wait_queue_t *new)
{
	unsigned long flags;
	unsigned int passed;

	spin_lock_irqsave(&head->lock, flags);
	passed = __add_wait_queue_prio_exclusive(head, new);

	spin_unlock_irqrestore(&head->lock, flags);

	return passed;
}


#endif
