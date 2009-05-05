#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>

#include <litmus/heap.h>

#include <linux/module.h>

/* cpu_entry_t - maintain the linked and scheduled state
 */
typedef struct  {
	int 			cpu;
	struct task_struct*	linked;		/* only RT tasks */
	int			picked;		/* linked was seen */
	struct task_struct*	scheduled;	/* only RT tasks */
	struct heap_node*	hn;
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, ghqedf_cpu_entries);

DEFINE_SPINLOCK(ghqedf_cpu_lock); /* synchronize access to cpu heap */

cpu_entry_t* ghqedf_cpus[NR_CPUS];

/* the cpus queue themselves according to priority in here */
static struct heap_node ghqedf_heap_node[NR_CPUS];
static struct heap      ghqedf_cpu_heap;

static rt_domain_t ghqedf; /* used only for the release queue */

struct subqueue {
	struct heap		queue;
	struct task_struct*	top;
	struct heap_node*	hn;
	spinlock_t		lock;
};

/* per-cpu sub queue */
//DEFINE_PER_CPU(struct subqueue, ghqedf_subqueue);

struct subqueue ghqedf_subqueue[NR_CPUS];

/* heap nodes for subqueue::hn field */
static struct heap_node ghqedf_subqueue_heap_node[NR_CPUS];

/* queue of sub queues */
struct heap master_queue;

/* re-use ready queue lock */
#define master_lock (ghqedf.ready_lock)

static int subqueue_higher_prio(struct heap_node *_a, struct heap_node *_b)
{
	struct subqueue *a, *b;
	a = _a->value;
	b = _b->value;
	return edf_higher_prio(a->top, b->top);
}

static void subqueues_init(void)
{
	int cpu;
	struct subqueue *q;

	heap_init(&master_queue);

	for_each_online_cpu(cpu) {
//		q = &per_cpu(ghqedf_subqueue, cpu);
		q = ghqedf_subqueue + cpu;
		heap_init(&q->queue);
		q->top = NULL;
		q->hn  = ghqedf_subqueue_heap_node + cpu;
		heap_node_init(&q->hn, q);
		spin_lock_init(&q->lock);
		heap_insert(subqueue_higher_prio, &master_queue, q->hn);
	}
}

static void __update_top(struct subqueue* q)
{
	struct heap_node *tmp;

	tmp = heap_peek(edf_ready_order, &q->queue);
	q->top = tmp ? tmp->value : NULL;
}

static void update_top(struct subqueue* q)
{
	spin_lock(&q->lock);
	__update_top(q);
	spin_unlock(&q->lock);
}

static void merge_into_ready_queue(struct heap *h)
{
//	struct subqueue *q = &__get_cpu_var(ghqedf_subqueue);
	struct subqueue *q = ghqedf_subqueue + smp_processor_id();
	struct heap_node *tmp;
	void *old_top;
	int changed_top = 0;

	spin_lock(&q->lock);
	tmp = heap_peek(edf_ready_order, &q->queue);
	old_top = tmp ? tmp->value : NULL;
	heap_union(edf_ready_order, &q->queue, h);
	/* is the new min the task that we just inserted? */
	changed_top = !old_top ||
		heap_peek(edf_ready_order, &q->queue)->value != old_top;
	spin_unlock(&q->lock);
	if (changed_top) {
		/* need to update master queue */
		spin_lock(&master_lock);
		/* If it is not in the heap then it is already
		 * being updated concurrently, so we skip it.
		 */
		if (likely(heap_node_in_heap(q->hn))) {
			heap_delete(subqueue_higher_prio, &master_queue, q->hn);
			update_top(q);
			heap_insert(subqueue_higher_prio, &master_queue, q->hn);
		} else
			TRACE("not updating subqueue top\n");
		spin_unlock(&master_lock);
	}
}

static void add_to_ready_queue(struct task_struct *t)
{
	struct heap tmp;

	TRACE_TASK(t, "adding to ready queue\n");
	heap_init(&tmp);
	heap_insert(edf_ready_order, &tmp, tsk_rt(t)->heap_node);
	merge_into_ready_queue(&tmp);
}


static int cpu_lower_prio(struct heap_node *_a, struct heap_node *_b)
{
	cpu_entry_t *a, *b;
	a = _a->value;
	b = _b->value;
	/* Note that a and b are inverted: we want the lowest-priority CPU at
	 * the top of the heap.
	 */
	return edf_higher_prio(b->linked, a->linked);
}

static void remove_from_cpu_heap(cpu_entry_t* entry)
{
	if (likely(heap_node_in_heap(entry->hn)))
		heap_delete(cpu_lower_prio, &ghqedf_cpu_heap, entry->hn);
}

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold ghqedf lock.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	remove_from_cpu_heap(entry);
	heap_insert(cpu_lower_prio, &ghqedf_cpu_heap, entry->hn);
}

/* caller must hold ghqedf lock */
static cpu_entry_t* lowest_prio_cpu(int take)
{
	struct heap_node* hn;
	if (take)
		hn = heap_take(cpu_lower_prio, &ghqedf_cpu_heap);
	else
		hn = heap_peek(cpu_lower_prio, &ghqedf_cpu_heap);
	return hn ? hn->value : NULL;
}


/* link_task_to_cpu - Update the link of a CPU.
 *                    Handles the case where the to-be-linked task is already
 *                    scheduled on a different CPU.
 */
static noinline void link_task_to_cpu(struct task_struct* linked,
				      cpu_entry_t *entry)
{
	cpu_entry_t *sched = NULL;
	struct task_struct* tmp;
	int on_cpu;

	BUG_ON(linked && !is_realtime(linked));

	/* Currently linked task is set to be unlinked. */
	if (entry->linked) {
		entry->linked->rt_param.linked_on = NO_CPU;
	}

	/* Link new task to CPU. */
	if (linked) {
		set_rt_flags(linked, RT_F_RUNNING);
		/* handle task is already scheduled somewhere! */
		on_cpu = linked->rt_param.scheduled_on;
		if (on_cpu != NO_CPU) {
			sched = &per_cpu(ghqedf_cpu_entries, on_cpu);
			/* this should only happen if not linked already */
			BUG_ON(sched->linked == linked);

			/* If we are already scheduled on the CPU to which we
			 * wanted to link, we don't need to do the swap --
			 * we just link ourselves to the CPU and depend on
			 * the caller to get things right.
			 *
			 * But only swap if the other node is in the queue.
			 * If it is not, then it is being updated
			 * concurrently and some other task was already
			 * picked for it.
			 */
			if (entry != sched && heap_node_in_heap(sched->hn)) {
				TRACE_TASK(linked,
					   "already scheduled on %d, "
					   "updating link.\n",
					   sched->cpu);
				tmp = sched->linked;
				linked->rt_param.linked_on = sched->cpu;
				sched->linked = linked;
				sched->picked = 1;
				update_cpu_position(sched);
				linked = tmp;
			}
		}
		if (linked) /* might be NULL due to swap */
			linked->rt_param.linked_on = entry->cpu;
	}
	entry->linked = linked;
	entry->picked = entry == sched; /* set to one if we linked to the
					 * the CPU that the task is
					 * executing on
					 */
	if (linked)
		TRACE_TASK(linked, "linked to %d.\n", entry->cpu);
	else
		TRACE("NULL linked to %d.\n", entry->cpu);
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold ghqedf_lock.
 */
static noinline void unlink(struct task_struct* t)
{
    	cpu_entry_t *entry;

	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(ghqedf_cpu_entries, t->rt_param.linked_on);
		t->rt_param.linked_on = NO_CPU;
		link_task_to_cpu(NULL, entry);
	}
}


/* preempt - force a CPU to reschedule
 */
static noinline void preempt(cpu_entry_t *entry)
{
	if (smp_processor_id() == entry->cpu)
		set_tsk_need_resched(current);
	else
		smp_send_reschedule(entry->cpu);
}

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold ghqedf_lock.
 *
 * call unlocked, but with preemptions disabled!
 */
static noinline void requeue(struct task_struct* task)
{
	if (is_released(task, litmus_clock()))
		add_to_ready_queue(task);
	else
		/* it has got to wait */
		add_release(&ghqedf, task);
}


static struct task_struct* take_if_preempt_required(cpu_entry_t* last)
{
	struct heap_node* tmp;
	struct subqueue* q;
	struct task_struct* t;
	int preempt_required = 0;

	spin_lock(&master_lock);
	tmp = heap_peek(subqueue_higher_prio, &master_queue);
	BUG_ON(!tmp); /* should be there */
	q = tmp->value;

	spin_lock(&q->lock);
	tmp = heap_peek(edf_ready_order, &q->queue);
	t = tmp ? tmp->value : NULL;
	preempt_required = edf_higher_prio(t, last->linked);
	if (preempt_required) {
		/* take it out */
		last = lowest_prio_cpu(1);
		spin_unlock(&ghqedf_cpu_lock);
		heap_delete(subqueue_higher_prio, &master_queue, q->hn);
	}
	/* drop lock master lock while we update subqueue */
	spin_unlock(&master_lock);

	if (preempt_required) {
		heap_delete(edf_ready_order, &q->queue, tmp);
		/* precompute, so that next lookup is O(1) */
		__update_top(q);
		spin_unlock(&q->lock);

		/* re-insert with new priority */
		spin_lock(&master_lock);
		/* update, with right locking order */
		update_top(q);
		heap_insert(subqueue_higher_prio, &master_queue, q->hn);
		spin_unlock(&master_lock);

		return t;
	} else {
		spin_unlock(&q->lock);
		return NULL;
	}
}


/* check for any necessary preemptions */
static void check_for_preemptions(void)
{
	int done = 0;
	unsigned long flags;
	struct task_struct *task, *unlinked;
	cpu_entry_t* last;


	local_irq_save(flags);
	while (!done) {
		unlinked = NULL;
		spin_lock(&ghqedf_cpu_lock);
		last = lowest_prio_cpu(0);
		if (likely(last)) {
			task = take_if_preempt_required(last);
			if (task) {
				TRACE_TASK(task, "removed from ready Q\n");
				/* cpu lock was dropped, reacquire */
				spin_lock(&ghqedf_cpu_lock);
				if (last->linked && !last->picked)
					/* can be requeued by us */
					unlinked = last->linked;
				TRACE("check_for_preemptions: "
				      "attempting to link task %d to %d\n",
				      task->pid, last->cpu);
				link_task_to_cpu(task, last);
				update_cpu_position(last);
			} else
				/* no preemption required */
				done = 1;
		} else
			/* all gone, being checked elsewhere? */
			done = 1;
		spin_unlock(&ghqedf_cpu_lock);
		if (unlinked)
			/* stick it back into the queue */
			requeue(unlinked);
		if (last && !done)
			/* we have a preemption, send IPI */
			preempt(last);
	}
	TRACE("done with preemption checking\n");
	local_irq_restore(flags);
}

/* ghqedf_job_arrival: task is either resumed or released
 * call only unlocked!
 */
static noinline void ghqedf_job_arrival(struct task_struct* task)
{
	requeue(task);
	check_for_preemptions();
}

static void ghqedf_release_jobs(rt_domain_t* rt, struct heap* tasks)
{
	unsigned long flags;

	TRACE("release_jobs() invoked\n");
	local_irq_save(flags);
	/* insert unlocked */
	merge_into_ready_queue(tasks);
	local_irq_restore(flags);
	check_for_preemptions();
}

/* caller holds ghqedf_lock */
static noinline int job_completion(cpu_entry_t* entry, int forced)
{

	struct task_struct *t = entry->scheduled;

	sched_trace_task_completion(t, forced);

	TRACE_TASK(t, "job_completion().\n");

	/* set flags */
	set_rt_flags(t, RT_F_SLEEP);
	/* prepare for next period */
	prepare_for_next_period(t);
	if (is_released(t, litmus_clock()))
		sched_trace_task_release(t);


	if (is_released(t, litmus_clock())){
		/* we changed the priority, see if we need to preempt */
		set_rt_flags(t, RT_F_RUNNING);
		update_cpu_position(entry);
		return 1;
	}
	else {
		/* it has got to wait */
		unlink(t);
		add_release(&ghqedf, t);
		return 0;
	}
}

/* ghqedf_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void ghqedf_tick(struct task_struct* t)
{
	if (is_realtime(t) && budget_exhausted(t))
		set_tsk_need_resched(t);
}

static struct task_struct* ghqedf_schedule(struct task_struct * prev)
{
	cpu_entry_t* entry = &__get_cpu_var(ghqedf_cpu_entries);
	int out_of_time, sleep, preempt, exists, blocks;
	struct task_struct* next = NULL;

	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (ghqedf.release_master == entry->cpu)
		return NULL;

//	TRACE_TASK(prev, "invoked ghqedf_schedule.\n");

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);

	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists && budget_exhausted(entry->scheduled);
	sleep	    = exists && get_rt_flags(entry->scheduled) == RT_F_SLEEP;

	spin_lock(&ghqedf_cpu_lock);

	preempt     = entry->scheduled != entry->linked;

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d sleep:%d preempt:%d "
			   "state:%d sig:%d\n",
			   blocks, out_of_time, sleep, preempt,
			   prev->state, signal_pending(prev));
	if (preempt && entry->linked)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);

	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		unlink(entry->scheduled);


	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this. Don't do a job completion if we block (can't have timers
	 * running for blocked jobs). Preemptions go first for the same reason.
	 */
	if ((out_of_time || sleep) && !blocks && !preempt) {
		if (job_completion(entry, !sleep)) {
			/* Task might stay with us.
			 * Drop locks and check for preemptions.
			 */
			spin_unlock(&ghqedf_cpu_lock);
			/* anything to update ? */
			check_for_preemptions();
			spin_lock(&ghqedf_cpu_lock);
			/* if something higher priority got linked,
			 * then we need to add the task into the
			 * ready queue (since it wasn't added by 
			 * check_for_preemptions b/c picked==1.
			 */
			if (entry->linked != prev)
				add_to_ready_queue(prev);
		}
	}

	/* Link pending task if we became unlinked.
	 * NOTE: Do not hold locks while performing ready queue updates
	 *       since we want concurrent access to the queue.
	 */
	if (!entry->linked) {
		if (exists)
			/* We are committed to descheduling; erase marker
			 * before we drop the lock.
			 */
			tsk_rt(prev)->scheduled_on = NO_CPU;
		spin_unlock(&ghqedf_cpu_lock);
		check_for_preemptions(); /* update links */
		spin_lock(&ghqedf_cpu_lock);
	}

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
	 */
	if (entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked) {
			entry->linked->rt_param.scheduled_on = entry->cpu;
			entry->picked = 1;
			next = entry->linked;
		}
		if (entry->scheduled)
			entry->scheduled->rt_param.scheduled_on = NO_CPU;
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	spin_unlock(&ghqedf_cpu_lock);
	if (exists && preempt && !blocks)
		/* stick preempted task back into the ready queue */
		ghqedf_job_arrival(prev);

	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());

	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void ghqedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = &__get_cpu_var(ghqedf_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
	TRACE_TASK(prev, "switched away from\n");
}


/*	Prepare a task for running in RT mode
 */
static void ghqedf_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;

	TRACE("ghqedf: task new %d\n", t->pid);

	spin_lock_irqsave(&ghqedf_cpu_lock, flags);

	/* setup job params */
	release_at(t, litmus_clock());

	if (running) {
		entry = &per_cpu(ghqedf_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);
		if (entry->cpu != ghqedf.release_master) {
			entry->scheduled = t;
			t->rt_param.scheduled_on = task_cpu(t);
		} else {
			preempt(entry);
			tsk_rt(t)->scheduled_on = NO_CPU;
		}
	} else {
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	tsk_rt(t)->linked_on          = NO_CPU;

	spin_unlock_irqrestore(&ghqedf_cpu_lock, flags);

	if (!running || entry->cpu == ghqedf.release_master)
		ghqedf_job_arrival(t);
}

static void ghqedf_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	spin_lock_irqsave(&ghqedf_cpu_lock, flags);
	now = litmus_clock();
	if (is_tardy(task, now)) {
		/* new sporadic release */
		release_at(task, now);
		sched_trace_task_release(task);
	}
	spin_unlock_irqrestore(&ghqedf_cpu_lock, flags);
	ghqedf_job_arrival(task);
}

static void ghqedf_task_block(struct task_struct *t)
{
	TRACE_TASK(t, "block at %llu\n", litmus_clock());
}

static void ghqedf_task_exit(struct task_struct * t)
{
	unsigned long flags;

	/* unlink if necessary */
	spin_lock_irqsave(&ghqedf_cpu_lock, flags);
	/* remove from CPU state, if necessary */
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		ghqedf_cpus[tsk_rt(t)->scheduled_on]->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	} else {
		/* FIXME: If t is currently queued, then we need to
		 *        dequeue it now; otherwise it will probably
		 *        cause a crash once it is dequeued.
		 */
		TRACE_TASK(t, "called ghqedf_task_exit(), "
			   "but is not scheduled!\n");
	}
	spin_unlock_irqrestore(&ghqedf_cpu_lock, flags);

        TRACE_TASK(t, "RIP\n");
}

static long ghqedf_admit_task(struct task_struct* tsk)
{
	return 0;
}


static long ghqedf_activate_plugin(void)
{
	int cpu;
	cpu_entry_t *entry;

	heap_init(&ghqedf_cpu_heap);
	subqueues_init();
	ghqedf.release_master = atomic_read(&release_master_cpu);

	for_each_online_cpu(cpu) {
		entry = &per_cpu(ghqedf_cpu_entries, cpu);
		heap_node_init(&entry->hn, entry);
		entry->linked    = NULL;
		entry->scheduled = NULL;
		entry->picked    = 0;
		if (cpu != ghqedf.release_master) {
			TRACE("G-EDF: Initializing CPU #%d.\n", cpu);
			update_cpu_position(entry);
		} else {
			TRACE("G-EDF: CPU %d is release master.\n", cpu);
		}
	}
	return 0;
}


/*	Plugin object	*/
static struct sched_plugin ghqedf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "GHQ-EDF",
	.finish_switch		= ghqedf_finish_switch,
	.tick			= ghqedf_tick,
	.task_new		= ghqedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= ghqedf_task_exit,
	.schedule		= ghqedf_schedule,
	.task_wake_up		= ghqedf_task_wake_up,
	.task_block		= ghqedf_task_block,
	.admit_task		= ghqedf_admit_task,
	.activate_plugin	= ghqedf_activate_plugin,
};


static int __init init_ghqedf(void)
{
	int cpu;
	cpu_entry_t *entry;

	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(ghqedf_cpu_entries, cpu);
		ghqedf_cpus[cpu] = entry;
		entry->cpu 	 = cpu;
		entry->hn        = &ghqedf_heap_node[cpu];
		heap_node_init(&entry->hn, entry);
	}
	edf_domain_init(&ghqedf, NULL, ghqedf_release_jobs);
	return register_sched_plugin(&ghqedf_plugin);
}


module_init(init_ghqedf);
