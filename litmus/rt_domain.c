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

#include <litmus/trace.h>

#include <litmus/heap.h>

static int dummy_resched(rt_domain_t *rt)
{
	return 0;
}

static int dummy_order(struct heap_node* a, struct heap_node* b)
{
	return 0;
}

/* default implementation: use default lock */
static void default_release_jobs(rt_domain_t* rt, struct heap* tasks)
{
	merge_ready(rt, tasks);
}

static unsigned int time2slot(lt_t time)
{
	return (unsigned int) time2quanta(time, FLOOR) % RELEASE_QUEUE_SLOTS;
}

int heap_earlier_release(struct heap_node *_a, struct heap_node *_b)
{
	struct release_heap *a = _a->value;
	struct release_heap *b = _b->value;
	return lt_before(a->release_time, b->release_time);
}

/* Caller most hold release lock.
 * Will return heap for given time. If no such heap exists prior to the invocation
 * it will be created.
 */
static struct release_heap* get_release_heap(rt_domain_t *rt, lt_t release_time)
{
	struct list_head* pos;
	struct release_heap* heap = NULL;
	struct release_heap* rh;
	unsigned int slot = time2slot(release_time);
	int inserted;

	/* initialize pos for the case that the list is empty */
	pos = rt->release_queue.slot[slot].next;
	list_for_each(pos, &rt->release_queue.slot[slot]) {
		rh = list_entry(pos, struct release_heap, list);
		if (release_time == rh->release_time) {
			/* perfect match -- this happens on hyperperiod
			 * boundaries
			 */
			heap = rh;
			break;
		} else if (lt_before(release_time, rh->release_time)) {
			/* we need to insert a new node since rh is
			 * already in the future
			 */
			break;
		}
	}
	if (!heap) {
		/* must create new node */
		/* FIXME: use a kmemcache_t */
		rh = kmalloc(sizeof(struct release_heap), GFP_ATOMIC);
		if (unlikely(!rh))
			/* Should be handled somehow.
			 * For now, let's just hope there is
			 * sufficient memory.
			 */
			panic("rt_domain: no more memory?");
		rh->release_time = release_time;
		heap_init(&rh->heap);
		list_add(&rh->list, pos->prev);
		inserted = heap_add(heap_earlier_release,
				    &rt->release_queue.rel_heap, rh,
				    GFP_ATOMIC);
		if (unlikely(!inserted))
			panic("rt_domain: no more heap memory?");
		heap = rh;
	}
	return heap;
}

static enum hrtimer_restart on_release_timer(struct hrtimer *timer)
{
	long flags;
	rt_domain_t *rt;
	struct release_heap* rh;
	struct heap tasks;
	struct list_head list, *pos, *safe;
	lt_t release = 0;
	int pending;
	int repeat;
	enum hrtimer_mode ret = HRTIMER_NORESTART;

	TS_RELEASE_START;

	INIT_LIST_HEAD(&list);
	heap_init(&tasks);

	rt = container_of(timer, rt_domain_t,
			 release_queue.timer);

	do {
		list_for_each_safe(pos, safe, &list) {
			rh = list_entry(pos, struct release_heap, list);
			heap_union(rt->order, &tasks, &rh->heap);
			list_del(pos);
			kfree(rh);
		}

		/* call release callback */
		rt->release_jobs(rt, &tasks);


		spin_lock_irqsave(&rt->release_lock, flags);
		while ((pending = next_release(rt, &release))) {
			if (lt_before(release, litmus_clock())) {
				/* pick for release */
				rh = heap_take_del(heap_earlier_release,
						   &rt->release_queue.rel_heap);
				list_move(&rh->list, &list);
			} else
				break;
		}
		repeat = !list_empty(&list);
		if (!repeat) {
			/* last iteration, setup timers, etc. */
			if (!pending) {
				rt->release_queue.timer_armed = 0;
				ret = HRTIMER_NORESTART;
			} else {
				rt->release_queue.timer_time = release;
				timer->expires = ns_to_ktime(release);
				ret = HRTIMER_RESTART;
			}
		}
		spin_unlock_irqrestore(&rt->release_lock, flags);
	} while (repeat);

	TS_RELEASE_END;

	return ret;
}

static void arm_release_timer(unsigned long _rt)
{
	rt_domain_t *rt = (rt_domain_t*) _rt;
	unsigned long flags;
	struct list_head list;
	struct list_head *pos, *safe;
	struct task_struct* t;
	struct release_heap* rh;
	int earlier, armed;
	lt_t release = 0;

	local_irq_save(flags);
	spin_lock(&rt->tobe_lock);
	list_replace_init(&rt->tobe_released, &list);
	spin_unlock(&rt->tobe_lock);

	/* We only have to defend against the ISR since norq callbacks
	 * are serialized.
	 */
	spin_lock(&rt->release_lock);

	list_for_each_safe(pos, safe, &list) {
		t = list_entry(pos, struct task_struct, rt_param.list);
		sched_trace_task_release(t);
		list_del(pos);
		rh = get_release_heap(rt, get_release(t));
		heap_add(rt->order, &rh->heap, t, GFP_ATOMIC);
	}

	next_release(rt, &release);
	armed   = rt->release_queue.timer_armed;
	earlier = lt_before(release, rt->release_queue.timer_time);
	/* We'll do the actual arming in a sec. The ISR doesn't care what these
	 * flags say, and it'll be true before another instance of this
	 * function can observe the flag due to the sequential nature of norq
	 * work.
	 */
	rt->release_queue.timer_armed = 1;
	rt->release_queue.timer_time  = release;
	spin_unlock(&rt->release_lock);
	if (!armed || earlier) {
		if (armed) {
			/* need to cancel first */
			hrtimer_cancel(&rt->release_queue.timer);
		}
		hrtimer_start(&rt->release_queue.timer,
			      ns_to_ktime(release),
			      HRTIMER_MODE_ABS);
	}
	local_irq_restore(flags);
}

void rt_domain_init(rt_domain_t *rt,
		    heap_prio_t order,
		    check_resched_needed_t check,
		    release_jobs_t release
		   )
{
	int i;

	BUG_ON(!rt);
	if (!check)
		check = dummy_resched;
	if (!release)
		release = default_release_jobs;
	if (!order)
		order = dummy_order;

	heap_init(&rt->ready_queue);
	INIT_LIST_HEAD(&rt->tobe_released);
	rt->release_queue.timer_armed = 0;
	for (i = 0; i < RELEASE_QUEUE_SLOTS; i++)
		INIT_LIST_HEAD(&rt->release_queue.slot[i]);

	hrtimer_init(&rt->release_queue.timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
        rt->release_queue.timer.function = on_release_timer;
#ifdef CONFIG_HIGH_RES_TIMERS
        rt->release_queue.timer.cb_mode = HRTIMER_CB_IRQSAFE;
#endif

	spin_lock_init(&rt->ready_lock);
	spin_lock_init(&rt->release_lock);
	spin_lock_init(&rt->tobe_lock);

	rt->check_resched 	= check;
	rt->release_jobs	= release;
	rt->order		= order;
	init_no_rqlock_work(&rt->arm_timer, arm_release_timer, (unsigned long) rt);
}

/* add_ready - add a real-time task to the rt ready queue. It must be runnable.
 * @new:       the newly released task
 */
void __add_ready(rt_domain_t* rt, struct task_struct *new)
{
	TRACE("rt: adding %s/%d (%llu, %llu) rel=%llu to ready queue at %llu\n",
	      new->comm, new->pid, get_exec_cost(new), get_rt_period(new),
	      get_release(new), litmus_clock());

	BUG_ON(heap_node_in_heap(tsk_rt(new)->heap_node));

	heap_insert(rt->order, &rt->ready_queue, tsk_rt(new)->heap_node);
	rt->check_resched(rt);
}

/* merge_ready - Add a sorted set of tasks to the rt ready queue. They must be runnable.
 * @tasks      - the newly released tasks
 */
void __merge_ready(rt_domain_t* rt, struct heap* tasks)
{
	heap_union(rt->order, &rt->ready_queue, tasks);
	rt->check_resched(rt);
}

/* add_release - add a real-time task to the rt release queue.
 * @task:        the sleeping task
 */
void __add_release(rt_domain_t* rt, struct task_struct *task)
{
	TRACE_TASK(task, "add_release(), rel=%llu\n", get_release(task));
	list_add(&tsk_rt(task)->list, &rt->tobe_released);
	task->rt_param.domain = rt;
	do_without_rqlock(&rt->arm_timer);
}

