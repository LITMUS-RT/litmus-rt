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
	struct task_struct*	scheduled;	/* only RT tasks */
	atomic_t		will_schedule;	/* prevent unneeded IPIs */
	struct heap_node*	hn;
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, gedf_cpu_entries);

cpu_entry_t* gedf_cpus[NR_CPUS];

#define set_will_schedule() \
	(atomic_set(&__get_cpu_var(gedf_cpu_entries).will_schedule, 1))
#define clear_will_schedule() \
	(atomic_set(&__get_cpu_var(gedf_cpu_entries).will_schedule, 0))
#define test_will_schedule(cpu) \
	(atomic_read(&per_cpu(gedf_cpu_entries, cpu).will_schedule))


#define NO_CPU 0xffffffff

/* the cpus queue themselves according to priority in here */
static struct heap_node gedf_heap_node[NR_CPUS];
static struct heap      gedf_cpu_heap;

static rt_domain_t gedf;
#define gedf_lock (gedf.ready_lock)


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

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold gedf lock.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	if (likely(heap_node_in_heap(entry->hn)))
		heap_delete(cpu_lower_prio, &gedf_cpu_heap, entry->hn);
	heap_insert(cpu_lower_prio, &gedf_cpu_heap, entry->hn);
}

/* caller must hold gedf lock */
static cpu_entry_t* lowest_prio_cpu(void)
{
	struct heap_node* hn;
	hn = heap_peek(cpu_lower_prio, &gedf_cpu_heap);
	return hn->value;
}


/* link_task_to_cpu - Update the link of a CPU.
 *                    Handles the case where the to-be-linked task is already
 *                    scheduled on a different CPU.
 */
static noinline void link_task_to_cpu(struct task_struct* linked,
				      cpu_entry_t *entry)
{
	cpu_entry_t *sched;
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
			sched = &per_cpu(gedf_cpu_entries, on_cpu);
			/* this should only happen if not linked already */
			BUG_ON(sched->linked == linked);

			/* If we are already scheduled on the CPU to which we
			 * wanted to link, we don't need to do the swap --
			 * we just link ourselves to the CPU and depend on
			 * the caller to get things right.
			 */
			if (entry != sched) {
				TRACE_TASK(linked,
					   "already scheduled on %d, updating link.\n",
					   sched->cpu);
				tmp = sched->linked;
				linked->rt_param.linked_on = sched->cpu;
				sched->linked = linked;
				update_cpu_position(sched);
				linked = tmp;
			}
		}
		if (linked) /* might be NULL due to swap */
			linked->rt_param.linked_on = entry->cpu;
	}
	entry->linked = linked;
	if (linked)
		TRACE_TASK(linked, "linked to %d.\n", entry->cpu);
	else
		TRACE("NULL linked to %d.\n", entry->cpu);
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold gedf_lock.
 */
static noinline void unlink(struct task_struct* t)
{
    	cpu_entry_t *entry;

	if (unlikely(!t)) {
		TRACE_BUG_ON(!t);
		return;
	}

	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(gedf_cpu_entries, t->rt_param.linked_on);
		t->rt_param.linked_on = NO_CPU;
		link_task_to_cpu(NULL, entry);
	} else if (is_queued(t)) {
		/* This is an interesting situation: t is scheduled,
		 * but was just recently unlinked.  It cannot be
		 * linked anywhere else (because then it would have
		 * been relinked to this CPU), thus it must be in some
		 * queue. We must remove it from the list in this
		 * case.
		 */
		remove(&gedf, t);
	}
}


/* preempt - force a CPU to reschedule
 */
static noinline void preempt(cpu_entry_t *entry)
{
	if (smp_processor_id() == entry->cpu) {
		set_tsk_need_resched(current);
	} else
		/* in case that it is a remote CPU we have to defer the
		 * the decision to the remote CPU
		 */
		if (!test_will_schedule(entry->cpu))
			smp_send_reschedule(entry->cpu);
}

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold gedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	BUG_ON(!task);
	/* sanity check before insertion */
	BUG_ON(is_queued(task));

	if (is_released(task, litmus_clock()))
		__add_ready(&gedf, task);
	else {
		/* it has got to wait */
		add_release(&gedf, task);
	}
}

/* check for any necessary preemptions */
static void check_for_preemptions(void)
{
	struct task_struct *task;
	cpu_entry_t* last;

	for(last = lowest_prio_cpu();
	    edf_preemption_needed(&gedf, last->linked);
	    last = lowest_prio_cpu()) {
		/* preemption necessary */
		task = __take_ready(&gedf);
		TRACE("check_for_preemptions: attempting to link task %d to %d\n",
		      task->pid, last->cpu);
		if (last->linked)
			requeue(last->linked);
		link_task_to_cpu(task, last);
		preempt(last);
	}
}

/* gedf_job_arrival: task is either resumed or released */
static noinline void gedf_job_arrival(struct task_struct* task)
{
	BUG_ON(!task);

	requeue(task);
	check_for_preemptions();
}

static void gedf_release_jobs(rt_domain_t* rt, struct heap* tasks)
{
	unsigned long flags;

	spin_lock_irqsave(&gedf_lock, flags);

	__merge_ready(rt, tasks);
	check_for_preemptions();

	spin_unlock_irqrestore(&gedf_lock, flags);
}

/* caller holds gedf_lock */
static noinline void job_completion(struct task_struct *t, int forced)
{
	BUG_ON(!t);

	sched_trace_task_completion(t, forced);

	TRACE_TASK(t, "job_completion().\n");

	/* set flags */
	set_rt_flags(t, RT_F_SLEEP);
	/* prepare for next period */
	prepare_for_next_period(t);
	if (is_released(t, litmus_clock()))
		sched_trace_task_release(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_running(t))
		gedf_job_arrival(t);
}

/* gedf_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void gedf_tick(struct task_struct* t)
{
	if (is_realtime(t) && budget_exhausted(t)) {
		set_tsk_need_resched(t);
		set_will_schedule();
	}
}

static struct task_struct* gedf_schedule(struct task_struct * prev)
{
	cpu_entry_t* entry = &__get_cpu_var(gedf_cpu_entries);
	int out_of_time, sleep, preempt, np, exists, blocks;
	struct task_struct* next = NULL;

	/* Will be released in finish_switch. */
	spin_lock(&gedf_lock);
	clear_will_schedule();

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);

	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists && budget_exhausted(entry->scheduled);
	sleep	    = exists && get_rt_flags(entry->scheduled) == RT_F_SLEEP;
	preempt     = entry->scheduled != entry->linked;

	TRACE_TASK(prev, "invoked gedf_schedule.\n");

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d sleep:%d preempt:%d "
			   "state:%d sig:%d\n",
			   blocks, out_of_time, sleep, preempt,
			   prev->state, signal_pending(prev));
	if (entry->linked && preempt)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);


	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		unlink(entry->scheduled);


	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this. Don't do a job completion if we block (can't have timers running
	 * for blocked jobs). Preemption go first for the same reason.
	 */
	if ((out_of_time || sleep) && !blocks && !preempt)
		job_completion(entry->scheduled, !sleep);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(&gedf), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
	 */
	if (entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked) {
			entry->linked->rt_param.scheduled_on = entry->cpu;
			next = entry->linked;
		}
		if (entry->scheduled) {
			/* not gonna be scheduled soon */
			entry->scheduled->rt_param.scheduled_on = NO_CPU;
			TRACE_TASK(entry->scheduled, "scheduled_on = NO_CPU\n");
		}
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	spin_unlock(&gedf_lock);

	TRACE("gedf_lock released, next=0x%p\n", next);


	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());


	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void gedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = &__get_cpu_var(gedf_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
	TRACE_TASK(prev, "switched away from\n");
}


/*	Prepare a task for running in RT mode
 */
static void gedf_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;

	TRACE("gsn edf: task new %d\n", t->pid);

	spin_lock_irqsave(&gedf_lock, flags);
	if (running) {
		entry = &per_cpu(gedf_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);
		entry->scheduled = t;
		t->rt_param.scheduled_on = task_cpu(t);
	} else
		t->rt_param.scheduled_on = NO_CPU;
	t->rt_param.linked_on          = NO_CPU;

	/* setup job params */
	release_at(t, litmus_clock());

	gedf_job_arrival(t);
	spin_unlock_irqrestore(&gedf_lock, flags);
}

static void gedf_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	spin_lock_irqsave(&gedf_lock, flags);
	/* We need to take suspensions because of semaphores into
	 * account! If a job resumes after being suspended due to acquiring
	 * a semaphore, it should never be treated as a new job release.
	 */
	if (get_rt_flags(task) == RT_F_EXIT_SEM) {
		set_rt_flags(task, RT_F_RUNNING);
	} else {
		now = litmus_clock();
		if (is_tardy(task, now)) {
			/* new sporadic release */
			release_at(task, now);
			sched_trace_task_release(task);
		}
		else if (task->time_slice)
			/* came back in time before deadline
			 */
			set_rt_flags(task, RT_F_RUNNING);
	}
	gedf_job_arrival(task);
	spin_unlock_irqrestore(&gedf_lock, flags);
}

static void gedf_task_block(struct task_struct *t)
{
	unsigned long flags;

	TRACE_TASK(t, "block at %llu\n", litmus_clock());

	/* unlink if necessary */
	spin_lock_irqsave(&gedf_lock, flags);
	unlink(t);
	spin_unlock_irqrestore(&gedf_lock, flags);

	BUG_ON(!is_realtime(t));
}


static void gedf_task_exit(struct task_struct * t)
{
	unsigned long flags;

	/* unlink if necessary */
	spin_lock_irqsave(&gedf_lock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		gedf_cpus[tsk_rt(t)->scheduled_on]->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	spin_unlock_irqrestore(&gedf_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}

static long gedf_admit_task(struct task_struct* tsk)
{
	return 0;
}


/*	Plugin object	*/
static struct sched_plugin gedf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "G-EDF",
	.finish_switch		= gedf_finish_switch,
	.tick			= gedf_tick,
	.task_new		= gedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= gedf_task_exit,
	.schedule		= gedf_schedule,
	.task_wake_up		= gedf_task_wake_up,
	.task_block		= gedf_task_block,
	.admit_task		= gedf_admit_task
};


static int __init init_gedf(void)
{
	int cpu;
	cpu_entry_t *entry;

	heap_init(&gedf_cpu_heap);
	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(gedf_cpu_entries, cpu);
		gedf_cpus[cpu] = entry;
		atomic_set(&entry->will_schedule, 0);
		entry->linked    = NULL;
		entry->scheduled = NULL;
		entry->cpu 	 = cpu;
		entry->hn        = &gedf_heap_node[cpu];
		heap_node_init(&entry->hn, entry);
	}
	edf_domain_init(&gedf, NULL, gedf_release_jobs);
	return register_sched_plugin(&gedf_plugin);
}


module_init(init_gedf);
