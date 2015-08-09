/*
 * litmus/rt_domain.c
 *
 * LITMUS real-time infrastructure. This file contains the
 * functions that manipulate RT domains. RT domains are an abstraction
 * of a ready queue and a release queue.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

#include <litmus/rt_domain.h>

#include <litmus/trace.h>

#include <litmus/bheap.h>

/* Uncomment when debugging timer races... */
#if 0
#define VTRACE_TASK TRACE_TASK
#define VTRACE TRACE
#else
#define VTRACE_TASK(t, fmt, args...) /* shut up */
#define VTRACE(fmt, args...) /* be quiet already */
#endif

static int dummy_resched(rt_domain_t *rt)
{
	return 0;
}

static int dummy_order(struct bheap_node* a, struct bheap_node* b)
{
	return 0;
}

/* default implementation: use default lock */
static void default_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	merge_ready(rt, tasks);
}

static unsigned int time2slot(lt_t time)
{
	return (unsigned int) time2quanta(time, FLOOR) % RELEASE_QUEUE_SLOTS;
}

static enum hrtimer_restart on_release_timer(struct hrtimer *timer)
{
	unsigned long flags;
	struct release_heap* rh;
	rh = container_of(timer, struct release_heap, timer);

	TS_RELEASE_LATENCY(rh->release_time);

	VTRACE("on_release_timer(0x%p) starts.\n", timer);

	TS_RELEASE_START;


	raw_spin_lock_irqsave(&rh->dom->release_lock, flags);
	VTRACE("CB has the release_lock 0x%p\n", &rh->dom->release_lock);
	/* remove from release queue */
	list_del(&rh->list);
	raw_spin_unlock_irqrestore(&rh->dom->release_lock, flags);
	VTRACE("CB returned release_lock 0x%p\n", &rh->dom->release_lock);

	/* call release callback */
	rh->dom->release_jobs(rh->dom, &rh->heap);
	/* WARNING: rh can be referenced from other CPUs from now on. */

	TS_RELEASE_END;

	VTRACE("on_release_timer(0x%p) ends.\n", timer);

	return  HRTIMER_NORESTART;
}

/* allocated in litmus.c */
struct kmem_cache * release_heap_cache;

struct release_heap* release_heap_alloc(int gfp_flags)
{
	struct release_heap* rh;
	rh= kmem_cache_alloc(release_heap_cache, gfp_flags);
	if (rh) {
		/* initialize timer */
		hrtimer_init(&rh->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
		rh->timer.function = on_release_timer;
	}
	return rh;
}

void release_heap_free(struct release_heap* rh)
{
	/* make sure timer is no longer in use */
	hrtimer_cancel(&rh->timer);
	kmem_cache_free(release_heap_cache, rh);
}

/* Caller must hold release lock.
 * Will return heap for given time. If no such heap exists prior to
 * the invocation it will be created.
 */
static struct release_heap* get_release_heap(rt_domain_t *rt,
					     struct task_struct* t,
					     int use_task_heap)
{
	struct list_head* pos;
	struct release_heap* heap = NULL;
	struct release_heap* rh;
	lt_t release_time = get_release(t);
	unsigned int slot = time2slot(release_time);

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
	if (!heap && use_task_heap) {
		/* use pre-allocated release heap */
		rh = tsk_rt(t)->rel_heap;

		rh->dom = rt;
		rh->release_time = release_time;

		/* add to release queue */
		list_add(&rh->list, pos->prev);
		heap = rh;
	}
	return heap;
}

static void reinit_release_heap(struct task_struct* t)
{
	struct release_heap* rh;

	/* use pre-allocated release heap */
	rh = tsk_rt(t)->rel_heap;

	/* Make sure it is safe to use.  The timer callback could still
	 * be executing on another CPU; hrtimer_cancel() will wait
	 * until the timer callback has completed.  However, under no
	 * circumstances should the timer be active (= yet to be
	 * triggered).
	 *
	 * WARNING: If the CPU still holds the release_lock at this point,
	 *          deadlock may occur!
	 */
	BUG_ON(hrtimer_cancel(&rh->timer));

	/* initialize */
	bheap_init(&rh->heap);
#ifdef CONFIG_RELEASE_MASTER
	atomic_set(&rh->info.state, HRTIMER_START_ON_INACTIVE);
#endif
}
/* arm_release_timer() - start local release timer or trigger
 *     remote timer (pull timer)
 *
 * Called by add_release() with:
 * - tobe_lock taken
 * - IRQ disabled
 */
#ifdef CONFIG_RELEASE_MASTER
#define arm_release_timer(t) arm_release_timer_on((t), NO_CPU)
static void arm_release_timer_on(rt_domain_t *_rt , int target_cpu)
#else
static void arm_release_timer(rt_domain_t *_rt)
#endif
{
	rt_domain_t *rt = _rt;
	struct list_head list;
	struct list_head *pos, *safe;
	struct task_struct* t;
	struct release_heap* rh;

	VTRACE("arm_release_timer() at %llu\n", litmus_clock());
	list_replace_init(&rt->tobe_released, &list);

	list_for_each_safe(pos, safe, &list) {
		/* pick task of work list */
		t = list_entry(pos, struct task_struct, rt_param.list);
		sched_trace_task_release(t);
		list_del(pos);

		/* put into release heap while holding release_lock */
		raw_spin_lock(&rt->release_lock);
		VTRACE_TASK(t, "I have the release_lock 0x%p\n", &rt->release_lock);

		rh = get_release_heap(rt, t, 0);
		if (!rh) {
			/* need to use our own, but drop lock first */
			raw_spin_unlock(&rt->release_lock);
			VTRACE_TASK(t, "Dropped release_lock 0x%p\n",
				    &rt->release_lock);

			reinit_release_heap(t);
			VTRACE_TASK(t, "release_heap ready\n");

			raw_spin_lock(&rt->release_lock);
			VTRACE_TASK(t, "Re-acquired release_lock 0x%p\n",
				    &rt->release_lock);

			rh = get_release_heap(rt, t, 1);
		}
		bheap_insert(rt->order, &rh->heap, tsk_rt(t)->heap_node);
		VTRACE_TASK(t, "arm_release_timer(): added to release heap\n");

		raw_spin_unlock(&rt->release_lock);
		VTRACE_TASK(t, "Returned the release_lock 0x%p\n", &rt->release_lock);

		/* To avoid arming the timer multiple times, we only let the
		 * owner do the arming (which is the "first" task to reference
		 * this release_heap anyway).
		 */
		if (rh == tsk_rt(t)->rel_heap) {
			VTRACE_TASK(t, "arming timer 0x%p\n", &rh->timer);

			if (!hrtimer_is_hres_active(&rh->timer)) {
				TRACE_TASK(t, "WARNING: no hires timer!!!\n");
			}

			/* we cannot arm the timer using hrtimer_start()
			 * as it may deadlock on rq->lock
			 *
			 * PINNED mode is ok on both local and remote CPU
			 */
#ifdef CONFIG_RELEASE_MASTER
			if (rt->release_master == NO_CPU &&
			    target_cpu == NO_CPU)
#endif
				__hrtimer_start_range_ns(&rh->timer,
						ns_to_ktime(rh->release_time),
						0, HRTIMER_MODE_ABS_PINNED, 0);
#ifdef CONFIG_RELEASE_MASTER
			else
				hrtimer_start_on(
					/* target_cpu overrides release master */
					(target_cpu != NO_CPU ?
					 target_cpu : rt->release_master),
					&rh->info, &rh->timer,
					ns_to_ktime(rh->release_time),
					HRTIMER_MODE_ABS_PINNED);
#endif
		} else
			VTRACE_TASK(t, "0x%p is not my timer\n", &rh->timer);
	}
}

void rt_domain_init(rt_domain_t *rt,
		    bheap_prio_t order,
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

#ifdef CONFIG_RELEASE_MASTER
	rt->release_master = NO_CPU;
#endif

	bheap_init(&rt->ready_queue);
	INIT_LIST_HEAD(&rt->tobe_released);
	for (i = 0; i < RELEASE_QUEUE_SLOTS; i++)
		INIT_LIST_HEAD(&rt->release_queue.slot[i]);

	raw_spin_lock_init(&rt->ready_lock);
	raw_spin_lock_init(&rt->release_lock);
	raw_spin_lock_init(&rt->tobe_lock);

	rt->check_resched 	= check;
	rt->release_jobs	= release;
	rt->order		= order;
}

/* add_ready - add a real-time task to the rt ready queue. It must be runnable.
 * @new:       the newly released task
 */
void __add_ready(rt_domain_t* rt, struct task_struct *new)
{
	TRACE("rt: adding %s/%d (%llu, %llu, %llu) rel=%llu "
		"to ready queue at %llu\n",
		new->comm, new->pid,
		get_exec_cost(new), get_rt_period(new), get_rt_relative_deadline(new),
		get_release(new), litmus_clock());

	BUG_ON(bheap_node_in_heap(tsk_rt(new)->heap_node));

	bheap_insert(rt->order, &rt->ready_queue, tsk_rt(new)->heap_node);
	rt->check_resched(rt);
}

/* merge_ready - Add a sorted set of tasks to the rt ready queue. They must be runnable.
 * @tasks      - the newly released tasks
 */
void __merge_ready(rt_domain_t* rt, struct bheap* tasks)
{
	bheap_union(rt->order, &rt->ready_queue, tasks);
	rt->check_resched(rt);
}


#ifdef CONFIG_RELEASE_MASTER
void __add_release_on(rt_domain_t* rt, struct task_struct *task,
		      int target_cpu)
{
	TRACE_TASK(task, "add_release_on(), rel=%llu, target=%d\n",
		   get_release(task), target_cpu);
	list_add(&tsk_rt(task)->list, &rt->tobe_released);
	task->rt_param.domain = rt;

	arm_release_timer_on(rt, target_cpu);
}
#endif

/* add_release - add a real-time task to the rt release queue.
 * @task:        the sleeping task
 */
void __add_release(rt_domain_t* rt, struct task_struct *task)
{
	TRACE_TASK(task, "add_release(), rel=%llu\n", get_release(task));
	list_add(&tsk_rt(task)->list, &rt->tobe_released);
	task->rt_param.domain = rt;

	arm_release_timer(rt);
}
