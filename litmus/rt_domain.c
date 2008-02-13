/*
 * kernel/rt_domain.c
 *
 * LITMUS real-time infrastructure. This file contains the
 * functions that manipulate RT domains. RT domains are an abstraction
 * of a ready queue and a release queue.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

#include <litmus/rt_domain.h>


static int dummy_resched(rt_domain_t *rt)
{
	return 0;
}

static int dummy_order(struct list_head* a, struct list_head* b)
{
	return 0;
}

int release_order(struct list_head* a, struct list_head* b)
{
	return earlier_release(
		list_entry(a, struct task_struct, rt_list),
		list_entry(b, struct task_struct, rt_list));
}


void rt_domain_init(rt_domain_t *rt,
		    check_resched_needed_t f,
		    list_cmp_t order)
{
	BUG_ON(!rt);
	if (!f)
		f = dummy_resched;
	if (!order)
		order = dummy_order;
	INIT_LIST_HEAD(&rt->ready_queue);
	INIT_LIST_HEAD(&rt->release_queue);
	rt->ready_lock  	= RW_LOCK_UNLOCKED;
	rt->release_lock 	= SPIN_LOCK_UNLOCKED;
	rt->check_resched 	= f;
	rt->order		= order;
}

/* add_ready - add a real-time task to the rt ready queue. It must be runnable.
 * @new:      the newly released task
 */
void __add_ready(rt_domain_t* rt, struct task_struct *new)
{
	TRACE("rt: adding %s/%d (%llu, %llu) to ready queue at %llu\n",
	      new->comm, new->pid, get_exec_cost(new), get_rt_period(new),
	      sched_clock());

	if (!list_insert(&new->rt_list, &rt->ready_queue, rt->order))
		rt->check_resched(rt);
}

struct task_struct* __take_ready(rt_domain_t* rt)
{
	struct task_struct *t = __peek_ready(rt);

	/* kick it out of the ready list */
	if (t)
		list_del(&t->rt_list);
	return t;
}

struct task_struct* __peek_ready(rt_domain_t* rt)
{
	if (!list_empty(&rt->ready_queue))
		return next_ready(rt);
	else
		return NULL;
}

/* add_release - add a real-time task to the rt release queue.
 * @task:        the sleeping task
 */
void __add_release(rt_domain_t* rt, struct task_struct *task)
{
	TRACE("rt: adding %s/%d (%llu, %llu) rel=%llu to release queue\n",
	      task->comm, task->pid, get_exec_cost(task), get_rt_period(task),
	      get_release(task));

	list_insert(&task->rt_list, &rt->release_queue, release_order);
}

void __release_pending(rt_domain_t* rt)
{
	struct list_head *pos, *save;
	struct task_struct   *queued;
	lt_t now = sched_clock();
	list_for_each_safe(pos, save, &rt->release_queue) {
		queued = list_entry(pos, struct task_struct, rt_list);
		if (likely(is_released(queued, now))) {
			/* this one is ready to go*/
			list_del(pos);
			set_rt_flags(queued, RT_F_RUNNING);

			sched_trace_job_release(queued);

			/* now it can be picked up */
			barrier();
			add_ready(rt, queued);
		}
		else
			/* the release queue is ordered */
			break;
	}
}

void try_release_pending(rt_domain_t* rt)
{
	unsigned long flags;

	if (spin_trylock_irqsave(&rt->release_lock, flags)) {
		__release_pending(rt);
		spin_unlock_irqrestore(&rt->release_lock, flags);
	}
}
