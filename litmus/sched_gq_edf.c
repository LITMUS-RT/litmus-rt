/* A quantum-based implementation of G-EDF.
 *
 * Based on GSN-EDF.
 */

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

/* cpu_state_t - maintain the linked and scheduled state
 */
typedef struct  {
	int 			cpu;
	struct task_struct*	linked;		/* only RT tasks */
	struct task_struct*	scheduled;	/* only RT tasks */
	struct task_struct*	absentee;	/* blocked quantum owner */
	struct heap_node*	hn;
} cpu_state_t;
DEFINE_PER_CPU(cpu_state_t, gq_cpu_entries);

cpu_state_t* gq_cpus[NR_CPUS];

/* the cpus queue themselves according to priority in here */
static struct heap_node gq_heap_node[NR_CPUS];
static struct heap      gq_cpu_heap;
/* jobs to be merged at the beginning of the next quantum */
static struct heap      gq_released_heap;


static rt_domain_t gqedf;
#define gq_lock (gqedf.ready_lock)

DEFINE_SPINLOCK(gq_release_lock);

static void preempt(cpu_state_t *entry)
{
	if (smp_processor_id() == entry->cpu)
		set_tsk_need_resched(current);
	else
		smp_send_reschedule(entry->cpu);
}

static int cpu_lower_prio(struct heap_node *_a, struct heap_node *_b)
{
	cpu_state_t *a, *b;
	a = _a->value;
	b = _b->value;
	/* Note that a and b are inverted: we want the lowest-priority CPU at
	 * the top of the heap.
	 */
	return edf_higher_prio(b->linked, a->linked);
}

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold gqedf lock.
 */
static void update_cpu_position(cpu_state_t *entry)
{
	if (likely(heap_node_in_heap(entry->hn)))
		heap_delete(cpu_lower_prio, &gq_cpu_heap, entry->hn);
	heap_insert(cpu_lower_prio, &gq_cpu_heap, entry->hn);
}

/* caller must hold gqedf lock */
static cpu_state_t* lowest_prio_cpu(void)
{
	struct heap_node* hn;
	hn = heap_peek(cpu_lower_prio, &gq_cpu_heap);
	return hn->value; //hn ? hn->value : NULL;
}

/* link_task_to_cpu - Update the link of a CPU.
 *                    Handles the case where the to-be-linked task is already
 *                    scheduled on a different CPU.
 */
static noinline void link_task_to_cpu(struct task_struct* linked,
				      cpu_state_t *entry)
{
	cpu_state_t *sched;
	struct task_struct* tmp;
	int on_cpu;

	BUG_ON(linked && !is_realtime(linked));
	/* don't relink tasks that are already linked */
	BUG_ON(linked && tsk_rt(linked)->linked_on != NO_CPU);

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
			sched = &per_cpu(gq_cpu_entries, on_cpu);
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
 *          where it was linked before. Must hold gq_lock.
 */
static noinline void unlink(struct task_struct* t)
{
    	cpu_state_t *entry;

	if (unlikely(!t)) {
		TRACE_BUG_ON(!t);
		return;
	}

	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(gq_cpu_entries, t->rt_param.linked_on);
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
		TRACE_TASK(t, "unlink() -> remove()\n");
		remove(&gqedf, t);
	}
}


/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold gq_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	BUG_ON(!task);
	/* sanity check before insertion */
	BUG_ON(is_queued(task));

	if (is_released(task, litmus_clock()))
		__add_ready(&gqedf, task);
	else
		/* it has got to wait */
		add_release(&gqedf, task);
}

/* check for any necessary preemptions */
static void link_highest_priority_jobs(void)
{
	struct task_struct *task;
	cpu_state_t* last;

	for(last = lowest_prio_cpu();
//	    last &&
	    edf_preemption_needed(&gqedf, last->linked);
	    last = lowest_prio_cpu()) {
		TRACE("last cpu:%d linked:%s/%d preempt:%d\n",
		      last->cpu,
		      last->linked ? last->linked->comm : "---",
		      last->linked ? last->linked->pid  : 0,
		      edf_preemption_needed(&gqedf, last->linked));
		/* preemption necessary */
		task = __take_ready(&gqedf);
		TRACE("attempting to link task %d to %d at %llu\n",
		      task->pid, last->cpu, litmus_clock());
		if (last->linked) {
			TRACE_TASK(last->linked, "requeued.\n");
			requeue(last->linked);
		}
		link_task_to_cpu(task, last);
	}
}

/* caller holds gq_lock */
static void job_completion(struct task_struct *t, int forced)
{

	sched_trace_task_completion(t, forced);

	TRACE_TASK(t, "job_completion(forced=%d), state:%d\n", forced,
		   t->state);

	/* prepare for next period */
	prepare_for_next_period(t);
	if (is_released(t, litmus_clock()))
		sched_trace_task_release(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_running(t))
		requeue(t);
	else
		TRACE_TASK(t, "job_completion(): not requeued, is not running. "
			   "state:%d\n", t->state);
}


static void gq_add_released_queue(struct task_struct *t)
{
	spin_lock(&gq_release_lock);
	heap_insert(edf_ready_order, &gq_released_heap, tsk_rt(t)->heap_node);
	spin_unlock(&gq_release_lock);
}

/* caller holds gq_lock, irqs are disabled */
static void merge_released_queue(void)
{
#ifdef CONFIG_SCHED_DEBUG_TRACE
	struct heap_node* hn;
	struct task_struct* t;
#endif

	spin_lock(&gq_release_lock);

#ifdef CONFIG_SCHED_DEBUG_TRACE
	/* do it individually (= slooow) 
	 * so that we can trace each merge
	 */


	while ((hn = heap_take(edf_ready_order, &gq_released_heap))) {
		t = (struct task_struct*) hn->value;
		TRACE_TASK(t, "merged into ready queue (is_released:%d)\n",
			   is_released(t, litmus_clock()));
		__add_ready(&gqedf, t);
	}
#else
	__merge_ready(&gqedf, &gq_released_heap);
#endif

	spin_unlock(&gq_release_lock);
}

/* gq_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void gq_tick(struct task_struct* t)
{
	unsigned long flags;
	cpu_state_t* entry;

	spin_lock_irqsave(&gq_lock, flags);
	entry =  &__get_cpu_var(gq_cpu_entries);
	entry->absentee = NULL;

	merge_released_queue();
	/* update linked if required */
	link_highest_priority_jobs();

	if (entry->linked != entry->scheduled ||
	    (is_realtime(t) && budget_exhausted(t)))
		/* let's reschedule */
		set_tsk_need_resched(t);

	spin_unlock_irqrestore(&gq_lock, flags);
}

static struct task_struct* gq_schedule(struct task_struct * prev)
{
	cpu_state_t* entry = &__get_cpu_var(gq_cpu_entries);
	int sleep, preempt, exists, blocks, out_of_time;
	struct task_struct* next = NULL;

	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (gqedf.release_master == entry->cpu)
		return NULL;

	spin_lock(&gq_lock);

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);
	BUG_ON(entry->scheduled && tsk_rt(entry->scheduled)->scheduled_on != entry->cpu);
	BUG_ON(entry->scheduled && tsk_rt(entry->scheduled)->scheduled_on != entry->cpu);

	/* Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists && budget_exhausted(entry->scheduled);
	sleep	    = exists && get_rt_flags(entry->scheduled) == RT_F_SLEEP;
	preempt     = entry->scheduled != entry->linked;

	BUG_ON(blocks && sleep);

	TRACE_TASK(prev, "invoked gq_schedule.\n");

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d sleep:%d preempt:%d "
			   "state:%d sig:%d out_of_time:%d\n",
			   blocks, sleep, preempt,
			   prev->state, signal_pending(prev),
			   out_of_time);
	if (entry->linked && preempt)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);


	if (blocks) {
		/* Record task identity so that the task
		 * can be rescheduled when it resumes,
		 * but only do so when prev has not been
		 * preempted anyway.
		 */
		if (!preempt) {
			entry->absentee = prev;
			tsk_rt(prev)->last_cpu = entry->cpu;
		}
		unlink(entry->scheduled);
	}

	if (sleep || out_of_time)
		job_completion(entry->scheduled, !sleep);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
	 */
	TRACE("gq: linked=%p scheduled=%p\n", entry->linked, entry->scheduled);
	if (entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked) {
			entry->linked->rt_param.scheduled_on = entry->cpu;
			next = entry->linked;
		}
		if (entry->scheduled) {
			/* kick this task off the CPU */
			entry->scheduled->rt_param.scheduled_on = NO_CPU;
			TRACE_TASK(entry->scheduled, "scheduled_on <- NO_CPU\n");
		}
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	spin_unlock(&gq_lock);

	TRACE("gq_lock released, next=0x%p\n", next);


	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());


	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void gq_finish_switch(struct task_struct *prev)
{
	cpu_state_t* 	entry = &__get_cpu_var(gq_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
	TRACE_TASK(prev, "switched away from\n");
}


/*	Prepare a task for running in RT mode
 */
static void gq_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 	flags;
	cpu_state_t* 	entry = NULL;
	int		on_rm = 0;

	spin_lock_irqsave(&gq_lock, flags);

	/* setup job params */
	release_at(t, litmus_clock());

	if (running) {
		entry = &per_cpu(gq_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);
		on_rm = entry->cpu == gqedf.release_master;
	}

	TRACE_TASK(t, "gq edf: task new (running:%d on_rm:%d)\n",
		   running, on_rm);

	if (running && on_rm)
		set_tsk_need_resched(t);

	if (running && !on_rm) {
		/* just leave it where it is, CPU was real-time idle */
		tsk_rt(t)->scheduled_on = task_cpu(t);
		tsk_rt(t)->linked_on = task_cpu(t);
		entry->scheduled = t;
		if (entry->linked != NULL) {
			/* Something raced and got assigned here.
			 * Kick it back into the queue, since t is
			 * already executing.
			 */
			tsk_rt(entry->linked)->linked_on = NO_CPU;
			__add_ready(&gqedf, entry->linked);
		}
		entry->linked = t;
	}

	if (!running || on_rm) {
		/* should be released properly */
		tsk_rt(t)->scheduled_on = NO_CPU;
		tsk_rt(t)->linked_on    = NO_CPU;
		gq_add_released_queue(t);
	}

	spin_unlock_irqrestore(&gq_lock, flags);
}

static void gq_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	cpu_state_t*  entry;
	lt_t now;

	spin_lock_irqsave(&gq_lock, flags);

	now = litmus_clock();
	if (is_tardy(task, now)) {
		TRACE_TASK(task, "wake_up => new release\n");
		/* Since task came back after its deadline, we
		 * assume that resuming indidates a new job release.
		 */
		release_at(task, now);
		sched_trace_task_release(task);
		gq_add_released_queue(task);
	} else if (is_released(task, now)) {
		set_rt_flags(task, RT_F_RUNNING);
		entry = &per_cpu(gq_cpu_entries, tsk_rt(task)->last_cpu);
		/* check if task is still the quantum owner on its CPU */
		if (entry->linked == NULL && entry->absentee == task) {
			TRACE_TASK(task, "wake_up => is quantum owner\n");
			/* can resume right away */
			link_task_to_cpu(task, entry);
			if (entry->scheduled != task)
				preempt(entry);
		} else {
			/* becomes eligible at next quantum */
			TRACE_TASK(task, "wake_up => released_queue\n");
			gq_add_released_queue(task);
		}
	} else {
		/* last suspension occurred together with a 
		 * job completion
		 */
		TRACE_TASK(task, "wake_up => not yet released!\n");
		requeue(task);
	}
	spin_unlock_irqrestore(&gq_lock, flags);
}

static void gq_task_block(struct task_struct *t)
{
	TRACE_TASK(t, "block at %llu\n", litmus_clock());
}


static void gq_task_exit(struct task_struct * t)
{
	unsigned long flags;

	/* unlink if necessary */
	spin_lock_irqsave(&gq_lock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		gq_cpus[tsk_rt(t)->scheduled_on]->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	spin_unlock_irqrestore(&gq_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}



static void gq_release_jobs(rt_domain_t* rt, struct heap* tasks)
{
	unsigned long flags;

	spin_lock_irqsave(&gq_release_lock, flags);
	TRACE("gq_release_jobs() at %llu\n", litmus_clock());

	/* save tasks to queue so that they can be merged on next quantum */
	heap_union(edf_ready_order, &gq_released_heap, tasks);

	spin_unlock_irqrestore(&gq_release_lock, flags);
}

static long gq_admit_task(struct task_struct* tsk)
{
	return 0;
}


static long gq_activate_plugin(void)
{
	int cpu;
	cpu_state_t *entry;

	heap_init(&gq_cpu_heap);
	heap_init(&gq_released_heap);
	gqedf.release_master = atomic_read(&release_master_cpu);


	for_each_online_cpu(cpu) {
		entry = &per_cpu(gq_cpu_entries, cpu);
		heap_node_init(&entry->hn, entry);
		entry->linked    = NULL;
		entry->scheduled = NULL;
		entry->absentee  = NULL;
		if (cpu != gqedf.release_master) {
			TRACE("GQ-EDF: Initializing CPU #%d.\n", cpu);
			update_cpu_position(entry);
		} else {
			TRACE("GQ-EDF: CPU %d is release master.\n", cpu);
		}
	}
	return 0;
}

/*	Plugin object	*/
static struct sched_plugin gq_edf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "GQ-EDF",
	.finish_switch		= gq_finish_switch,
	.tick			= gq_tick,
	.task_new		= gq_task_new,
	.complete_job		= complete_job,
	.task_exit		= gq_task_exit,
	.schedule		= gq_schedule,
	.task_wake_up		= gq_task_wake_up,
	.task_block		= gq_task_block,
	.admit_task		= gq_admit_task,
	.activate_plugin	= gq_activate_plugin,
};


static int __init init_gq_edf(void)
{
	int cpu;
	cpu_state_t *entry;

	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(gq_cpu_entries, cpu);
		gq_cpus[cpu] = entry;
		entry->cpu 	 = cpu;
		entry->hn        = &gq_heap_node[cpu];
		heap_node_init(&entry->hn, entry);
	}
	edf_domain_init(&gqedf, NULL, gq_release_jobs);
	return register_sched_plugin(&gq_edf_plugin);
}


module_init(init_gq_edf);
