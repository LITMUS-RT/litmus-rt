/* CLEANUP: Add comments and make it less messy.
 *
 */

#ifndef __UNC_RT_DOMAIN_H__
#define __UNC_RT_DOMAIN_H__

#include <litmus/norqlock.h>
#include <litmus/heap.h>

#define RELEASE_QUEUE_SLOTS 127 /* prime */

struct _rt_domain;

typedef int (*check_resched_needed_t)(struct _rt_domain *rt);
typedef void (*release_jobs_t)(struct _rt_domain *rt, struct heap* tasks);

int heap_earlier_release(struct heap_node *_a, struct heap_node *_b);

struct release_heap {
	struct list_head		list;
	lt_t				release_time;
	struct heap			heap;
};

struct release_queue {
	/* each slot maintains a list of release heaps sorted by release time */
	struct list_head		slot[RELEASE_QUEUE_SLOTS];
	/* the heap of heaps orderd by release time */
	struct heap			rel_heap;
	/* the actual timer used to trigger releases */
	struct hrtimer			timer;
	/* used to determine when to start the timer */
	int				timer_armed;
	/* when will it go off? */
	lt_t				timer_time;
};

typedef struct _rt_domain {
	struct no_rqlock_work		arm_timer;

	/* runnable rt tasks are in here */
	spinlock_t 			ready_lock;
	struct heap	 		ready_queue;

	/* real-time tasks waiting for release are in here */
	spinlock_t 			release_lock;
	struct release_queue 		release_queue;

	/* for moving tasks to the release queue */
	spinlock_t			tobe_lock;
	struct list_head		tobe_released;

	/* how do we check if we need to kick another CPU? */
	check_resched_needed_t		check_resched;

	/* how do we release a job? */
	release_jobs_t			release_jobs;

	/* how are tasks ordered in the ready queue? */
	heap_prio_t			order;
} rt_domain_t;

/* caller must hold release_lock */
static inline int next_release(rt_domain_t *rt, lt_t *time)
{
	struct heap_node* top = heap_peek(heap_earlier_release,
					  &rt->release_queue.rel_heap);
	if (top)
		*time = ((struct release_heap*) top->value)->release_time;
	return top != NULL;
}

static inline struct task_struct* __next_ready(rt_domain_t* rt)
{
	struct heap_node *hn = heap_peek(rt->order, &rt->ready_queue);
	if (hn)
		return heap2task(hn);
	else
		return NULL;
}

void rt_domain_init(rt_domain_t *rt, heap_prio_t order,
		    check_resched_needed_t check,
		    release_jobs_t relase);

void __add_ready(rt_domain_t* rt, struct task_struct *new);
void __merge_ready(rt_domain_t* rt, struct heap *tasks);
void __add_release(rt_domain_t* rt, struct task_struct *task);

static inline struct task_struct* __take_ready(rt_domain_t* rt)
{
	struct heap_node* hn = heap_take(rt->order, &rt->ready_queue);
	if (hn)
		return heap2task(hn);
	else
		return NULL;
}

static inline struct task_struct* __peek_ready(rt_domain_t* rt)
{
	struct heap_node* hn = heap_peek(rt->order, &rt->ready_queue);
	if (hn)
		return heap2task(hn);
	else
		return NULL;
}

static inline int  is_queued(struct task_struct *t)
{
	return heap_node_in_heap(tsk_rt(t)->heap_node);
}

static inline void remove(rt_domain_t* rt, struct task_struct *t)
{
	heap_delete(rt->order, &rt->ready_queue, tsk_rt(t)->heap_node);
}

static inline void add_ready(rt_domain_t* rt, struct task_struct *new)
{
	unsigned long flags;
	/* first we need the write lock for rt_ready_queue */
	spin_lock_irqsave(&rt->ready_lock, flags);
	__add_ready(rt, new);
	spin_unlock_irqrestore(&rt->ready_lock, flags);
}

static inline void merge_ready(rt_domain_t* rt, struct heap* tasks)
{
	unsigned long flags;
	spin_lock_irqsave(&rt->ready_lock, flags);
	__merge_ready(rt, tasks);
	spin_unlock_irqrestore(&rt->ready_lock, flags);
}

static inline struct task_struct* take_ready(rt_domain_t* rt)
{
	unsigned long flags;
	struct task_struct* ret;
	/* first we need the write lock for rt_ready_queue */
	spin_lock_irqsave(&rt->ready_lock, flags);
	ret = __take_ready(rt);
	spin_unlock_irqrestore(&rt->ready_lock, flags);
	return ret;
}


static inline void add_release(rt_domain_t* rt, struct task_struct *task)
{
	unsigned long flags;
	/* first we need the write lock for rt_ready_queue */
	spin_lock_irqsave(&rt->tobe_lock, flags);
	__add_release(rt, task);
	spin_unlock_irqrestore(&rt->tobe_lock, flags);
}

static inline int __jobs_pending(rt_domain_t* rt)
{
	return !heap_empty(&rt->ready_queue);
}

static inline int jobs_pending(rt_domain_t* rt)
{
	unsigned long flags;
	int ret;
	/* first we need the write lock for rt_ready_queue */
	spin_lock_irqsave(&rt->ready_lock, flags);
	ret = !heap_empty(&rt->ready_queue);
	spin_unlock_irqrestore(&rt->ready_lock, flags);
	return ret;
}

#endif
