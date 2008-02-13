/* CLEANUP: Add comments and make it less messy.
 *
 */

#ifndef __UNC_RT_DOMAIN_H__
#define __UNC_RT_DOMAIN_H__

struct _rt_domain;

typedef int (*check_resched_needed_t)(struct _rt_domain *rt);
typedef void (*release_at_t)(struct task_struct *t, lt_t start);

typedef struct _rt_domain {
	/* runnable rt tasks are in here */
	rwlock_t 			ready_lock;
	struct list_head 		ready_queue;

	/* real-time tasks waiting for release are in here */
	spinlock_t 			release_lock;
	struct list_head 		release_queue;

	/* how do we check if we need to kick another CPU? */
	check_resched_needed_t		check_resched;

	/* how are tasks ordered in the ready queue? */
	list_cmp_t			order;
} rt_domain_t;

#define next_ready(rt) \
	(list_entry((rt)->ready_queue.next, struct task_struct, rt_list))

#define ready_jobs_pending(rt) \
	(!list_empty(&(rt)->ready_queue))

void rt_domain_init(rt_domain_t *rt, check_resched_needed_t f,
		    list_cmp_t order);

void __add_ready(rt_domain_t* rt, struct task_struct *new);
void __add_release(rt_domain_t* rt, struct task_struct *task);

struct task_struct* __take_ready(rt_domain_t* rt);
struct task_struct* __peek_ready(rt_domain_t* rt);

void try_release_pending(rt_domain_t* rt);
void __release_pending(rt_domain_t* rt);

static inline void add_ready(rt_domain_t* rt, struct task_struct *new)
{
	unsigned long flags;
	/* first we need the write lock for rt_ready_queue */
	write_lock_irqsave(&rt->ready_lock, flags);
	__add_ready(rt, new);
	write_unlock_irqrestore(&rt->ready_lock, flags);
}

static inline struct task_struct* take_ready(rt_domain_t* rt)
{
	unsigned long flags;
	struct task_struct* ret;
	/* first we need the write lock for rt_ready_queue */
	write_lock_irqsave(&rt->ready_lock, flags);
	ret = __take_ready(rt);
	write_unlock_irqrestore(&rt->ready_lock, flags);
	return ret;
}


static inline void add_release(rt_domain_t* rt, struct task_struct *task)
{
	unsigned long flags;
	/* first we need the write lock for rt_ready_queue */
	spin_lock_irqsave(&rt->release_lock, flags);
	__add_release(rt, task);
	spin_unlock_irqrestore(&rt->release_lock, flags);
}

static inline int __jobs_pending(rt_domain_t* rt)
{
	return !list_empty(&rt->ready_queue);
}

static inline int jobs_pending(rt_domain_t* rt)
{
	unsigned long flags;
	int ret;
	/* first we need the write lock for rt_ready_queue */
	read_lock_irqsave(&rt->ready_lock, flags);
	ret = __jobs_pending(rt);
	read_unlock_irqrestore(&rt->ready_lock, flags);
	return ret;
}


#endif
