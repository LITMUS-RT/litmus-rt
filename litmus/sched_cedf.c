/*
 * kernel/sched_cedf.c
 *
 * Implementation of the Clustered EDF (C-EDF) scheduling algorithm.
 * Linking is included so that support for synchronization (e.g., through
 * the implementation of a "CSN-EDF" algorithm) can be added later if desired.
 *
 * This version uses the simple approach and serializes all scheduling
 * decisions by the use of a queue lock. This is probably not the
 * best way to do it, but it should suffice for now.
 */

#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>

#include <linux/module.h>

/* Overview of C-EDF operations.
 *
 * link_task_to_cpu(T, cpu) 	- Low-level operation to update the linkage
 *                                structure (NOT the actually scheduled
 *                                task). If there is another linked task To
 *                                already it will set To->linked_on = NO_CPU
 *                                (thereby removing its association with this
 *                                CPU). However, it will not requeue the
 *                                previously linked task (if any). It will set
 *                                T's state to RT_F_RUNNING and check whether
 *                                it is already running somewhere else. If T
 *                                is scheduled somewhere else it will link
 *                                it to that CPU instead (and pull the linked
 *                                task to cpu). T may be NULL.
 *
 * unlink(T)			- Unlink removes T from all scheduler data
 *                                structures. If it is linked to some CPU it
 *                                will link NULL to that CPU. If it is
 *                                currently queued in the cedf queue for
 *                                a partition, it will be removed from
 *                                the rt_domain. It is safe to call
 *                                unlink(T) if T is not linked. T may not
 *                                be NULL.
 *
 * requeue(T)			- Requeue will insert T into the appropriate
 *                                queue. If the system is in real-time mode and
 *                                the T is released already, it will go into the
 *                                ready queue. If the system is not in
 *                                real-time mode is T, then T will go into the
 *                                release queue. If T's release time is in the
 *                                future, it will go into the release
 *                                queue. That means that T's release time/job
 *                                no/etc. has to be updated before requeue(T) is
 *                                called. It is not safe to call requeue(T)
 *                                when T is already queued. T may not be NULL.
 *
 * cedf_job_arrival(T)		- This is the catch-all function when T enters
 *                                the system after either a suspension or at a
 *                                job release. It will queue T (which means it
 *                                is not safe to call cedf_job_arrival(T) if
 *                                T is already queued) and then check whether a
 *                                preemption is necessary. If a preemption is
 *                                necessary it will update the linkage
 *                                accordingly and cause scheduled to be called
 *                                (either with an IPI or need_resched). It is
 *                                safe to call cedf_job_arrival(T) if T's
 *                                next job has not been actually released yet
 *                                (release time in the future). T will be put
 *                                on the release queue in that case.
 *
 * job_completion(T)		- Take care of everything that needs to be done
 *                                to prepare T for its next release and place
 *                                it in the right queue with
 *                                cedf_job_arrival().
 *
 *
 * When we now that T is linked to CPU then link_task_to_cpu(NULL, CPU) is
 * equivalent to unlink(T). Note that if you unlink a task from a CPU none of
 * the functions will automatically propagate pending task from the ready queue
 * to a linked task. This is the job of the calling function ( by means of
 * __take_ready).
 */

/* cpu_entry_t - maintain the linked and scheduled state
 */
typedef struct  {
	int 			cpu;
	struct task_struct*	linked;		/* only RT tasks */
	struct task_struct*	scheduled;	/* only RT tasks */
	struct list_head	list;
	atomic_t		will_schedule;	/* prevent unneeded IPIs */
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, cedf_cpu_entries);

cpu_entry_t* cedf_cpu_entries_array[NR_CPUS];

#define set_will_schedule() \
	(atomic_set(&__get_cpu_var(cedf_cpu_entries).will_schedule, 1))
#define clear_will_schedule() \
	(atomic_set(&__get_cpu_var(cedf_cpu_entries).will_schedule, 0))
#define test_will_schedule(cpu) \
	(atomic_read(&per_cpu(cedf_cpu_entries, cpu).will_schedule))

#define NO_CPU 0xffffffff

/* Cluster size -- currently four. This is a variable to allow for
 * the possibility of changing the cluster size online in the future.
 */
int cluster_size = 4;

typedef struct {
	rt_domain_t 		domain;
	int          		first_cpu;
	int			last_cpu;

	/* the cpus queue themselves according to priority in here */
	struct list_head	cedf_cpu_queue;

	/* per-partition spinlock: protects the domain and
	 * serializes scheduling decisions
	 */
#define slock domain.ready_lock
} cedf_domain_t;

DEFINE_PER_CPU(cedf_domain_t*, cedf_domains) = NULL;

cedf_domain_t* cedf_domains_array[NR_CPUS];


/* These are defined similarly to partitioning, except that a
 * tasks partition is any cpu of the cluster to which it
 * is assigned, typically the lowest-numbered cpu.
 */
#define local_edf		(&__get_cpu_var(cedf_domains)->domain)
#define local_cedf		__get_cpu_var(cedf_domains)
#define remote_edf(cpu)		(&per_cpu(cedf_domains, cpu)->domain)
#define remote_cedf(cpu)	per_cpu(cedf_domains, cpu)
#define task_edf(task)		remote_edf(get_partition(task))
#define task_cedf(task)		remote_cedf(get_partition(task))

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold cedf lock.
 *
 *						This really should be a heap.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	cpu_entry_t *other;
	struct list_head *cedf_cpu_queue =
		&(remote_cedf(entry->cpu))->cedf_cpu_queue;
	struct list_head *pos;

	BUG_ON(!cedf_cpu_queue);

	if (likely(in_list(&entry->list)))
		list_del(&entry->list);
	/* if we do not execute real-time jobs we just move
	 * to the end of the queue
	 */
	if (entry->linked) {
		list_for_each(pos, cedf_cpu_queue) {
			other = list_entry(pos, cpu_entry_t, list);
			if (edf_higher_prio(entry->linked, other->linked)) {
				__list_add(&entry->list, pos->prev, pos);
				return;
			}
		}
	}
	/* if we get this far we have the lowest priority job */
	list_add_tail(&entry->list, cedf_cpu_queue);
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

	/* Cannot link task to a CPU that doesn't belong to its partition... */
	BUG_ON(linked && remote_cedf(entry->cpu) != task_cedf(linked));

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
			sched = &per_cpu(cedf_cpu_entries, on_cpu);
			/* this should only happen if not linked already */
			BUG_ON(sched->linked == linked);

			/* If we are already scheduled on the CPU to which we
			 * wanted to link, we don't need to do the swap --
			 * we just link ourselves to the CPU and depend on
			 * the caller to get things right.
			 */
			if (entry != sched) {
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

	if (entry->linked)
		TRACE_TASK(entry->linked, "linked to CPU %d, state:%d\n",
			   entry->cpu, entry->linked->state);
	else
		TRACE("NULL linked to CPU %d\n", entry->cpu);

	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold cedf_lock.
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
		entry = &per_cpu(cedf_cpu_entries, t->rt_param.linked_on);
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
		remove(task_edf(t), t);
	}
}


/* preempt - force a CPU to reschedule
 */
static noinline void preempt(cpu_entry_t *entry)
{
	/* We cannot make the is_np() decision here if it is a remote CPU
	 * because requesting exit_np() requires that we currently use the
	 * address space of the task. Thus, in the remote case we just send
	 * the IPI and let schedule() handle the problem.
	 */

	if (smp_processor_id() == entry->cpu) {
		if (entry->scheduled && is_np(entry->scheduled))
			request_exit_np(entry->scheduled);
		else
			set_tsk_need_resched(current);
	} else
		/* in case that it is a remote CPU we have to defer the
		 * the decision to the remote CPU
		 * FIXME: We could save a few IPI's here if we leave the flag
		 * set when we are waiting for a np_exit().
		 */
		if (!test_will_schedule(entry->cpu))
			smp_send_reschedule(entry->cpu);
}

/* requeue - Put an unlinked task into c-edf domain.
 *           Caller must hold cedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	cedf_domain_t* cedf;
	rt_domain_t* edf;

	BUG_ON(!task);
	/* sanity check rt_list before insertion */
	BUG_ON(is_queued(task));

	/* Get correct real-time domain. */
	cedf = task_cedf(task);
	edf = &cedf->domain;

	if (get_rt_flags(task) == RT_F_SLEEP) {
		/* this task has expired
		 * _schedule has already taken care of updating
		 * the release and
		 * deadline. We just must check if it has been released.
		 */
		if (is_released(task, litmus_clock()))
			__add_ready(edf, task);
		else {
			/* it has got to wait */
			add_release(edf, task);
		}

	} else
		/* this is a forced preemption
		 * thus the task stays in the ready_queue
		 * we only must make it available to others
		 */
		__add_ready(edf, task);
}

static void check_for_preemptions(cedf_domain_t* cedf)
{
	cpu_entry_t *last;
	struct task_struct *task;
	struct list_head *cedf_cpu_queue;
	cedf_cpu_queue = &cedf->cedf_cpu_queue;

	for(last = list_entry(cedf_cpu_queue->prev, cpu_entry_t, list);
	    edf_preemption_needed(&cedf->domain, last->linked);
	    last = list_entry(cedf_cpu_queue->prev, cpu_entry_t, list)) {
		/* preemption necessary */
		task = __take_ready(&cedf->domain);
		TRACE("check_for_preemptions: task %d linked to %d, state:%d\n",
		      task->pid, last->cpu, task->state);
		if (last->linked)
			requeue(last->linked);
		link_task_to_cpu(task, last);
		preempt(last);
	}

}

/* cedf_job_arrival: task is either resumed or released */
static noinline void cedf_job_arrival(struct task_struct* task)
{
	cedf_domain_t* cedf;
	rt_domain_t* edf;

	BUG_ON(!task);

	/* Get correct real-time domain. */
	cedf = task_cedf(task);
	edf = &cedf->domain;

	/* first queue arriving job */
	requeue(task);

	/* then check for any necessary preemptions */
	check_for_preemptions(cedf);
}

/* check for current job releases */
static void cedf_release_jobs(rt_domain_t* rt, struct heap* tasks)
{
        cedf_domain_t*          cedf = container_of(rt, cedf_domain_t, domain);
	unsigned long 		flags;

	spin_lock_irqsave(&cedf->slock, flags);

	__merge_ready(&cedf->domain, tasks);
	check_for_preemptions(cedf);
	spin_unlock_irqrestore(&cedf->slock, flags);
}

/* cedf_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void cedf_tick(struct task_struct* t)
{
	BUG_ON(!t);

	if (is_realtime(t) && budget_exhausted(t)) {
		if (!is_np(t)) {
			/* np tasks will be preempted when they become
			 * preemptable again
			 */
			set_tsk_need_resched(t);
			set_will_schedule();
			TRACE("cedf_scheduler_tick: "
			      "%d is preemptable (state:%d) "
			      " => FORCE_RESCHED\n", t->pid, t->state);
		} else {
			TRACE("cedf_scheduler_tick: "
			      "%d is non-preemptable (state:%d), "
			      "preemption delayed.\n", t->pid, t->state);
			request_exit_np(t);
		}
	}
}

/* caller holds cedf_lock */
static noinline void job_completion(struct task_struct *t)
{
	BUG_ON(!t);

	sched_trace_job_completion(t);

	TRACE_TASK(t, "job_completion(). [state:%d]\n", t->state);

	/* set flags */
	set_rt_flags(t, RT_F_SLEEP);
	/* prepare for next period */
	prepare_for_next_period(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_running(t))
		cedf_job_arrival(t);
}

/* Getting schedule() right is a bit tricky. schedule() may not make any
 * assumptions on the state of the current task since it may be called for a
 * number of reasons. The reasons include a scheduler_tick() determined that it
 * was necessary, because sys_exit_np() was called, because some Linux
 * subsystem determined so, or even (in the worst case) because there is a bug
 * hidden somewhere. Thus, we must take extreme care to determine what the
 * current state is.
 *
 * The CPU could currently be scheduling a task (or not), be linked (or not).
 *
 * The following assertions for the scheduled task could hold:
 *
 *      - !is_running(scheduled)        // the job blocks
 *	- scheduled->timeslice == 0	// the job completed (forcefully)
 *	- get_rt_flag() == RT_F_SLEEP	// the job completed (by syscall)
 * 	- linked != scheduled		// we need to reschedule (for any reason)
 * 	- is_np(scheduled)		// rescheduling must be delayed,
 *					   sys_exit_np must be requested
 *
 * Any of these can occur together.
 */
static struct task_struct* cedf_schedule(struct task_struct * prev)
{
	cedf_domain_t* 		cedf = local_cedf;
	rt_domain_t*		edf  = &cedf->domain;
	cpu_entry_t* 		entry = &__get_cpu_var(cedf_cpu_entries);
	int 			out_of_time, sleep, preempt, np,
				exists, blocks;
	struct task_struct* 	next = NULL;

	BUG_ON(!prev);
	BUG_ON(!cedf);
	BUG_ON(!edf);
	BUG_ON(!entry);
	BUG_ON(cedf != remote_cedf(entry->cpu));
	BUG_ON(is_realtime(prev) && cedf != task_cedf(prev));

	/* Will be released in finish_switch. */
	spin_lock(&cedf->slock);
	clear_will_schedule();

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);

	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists && budget_exhausted(entry->scheduled);
	np 	    = exists && is_np(entry->scheduled);
	sleep	    = exists && get_rt_flags(entry->scheduled) == RT_F_SLEEP;
	preempt     = entry->scheduled != entry->linked;

	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		unlink(entry->scheduled);

	/* Request a sys_exit_np() call if we would like to preempt but cannot.
	 * We need to make sure to update the link structure anyway in case
	 * that we are still linked. Multiple calls to request_exit_np() don't
	 * hurt.
	 */
	if (np && (out_of_time || preempt || sleep)) {
		unlink(entry->scheduled);
		request_exit_np(entry->scheduled);
	}

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this. Don't do a job completion if blocks (can't have timers
	 * running for blocked jobs). Preemption go first for the same reason.
	 */
	if (!np && (out_of_time || sleep) && !blocks && !preempt)
		job_completion(entry->scheduled);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(edf), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked different from scheduled select linked as next.
	 */
	if ((!np || blocks) &&
	    entry->linked != entry->scheduled) {
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
		/* Only override Linux scheduler if we have real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	spin_unlock(&cedf->slock);

	return next;
}

/* _finish_switch - we just finished the switch away from prev
 */
static void cedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* entry = &__get_cpu_var(cedf_cpu_entries);

	BUG_ON(!prev);
	BUG_ON(!entry);

	entry->scheduled = is_realtime(current) ? current : NULL;
}

/*	Prepare a task for running in RT mode
 */
static void cedf_task_new(struct task_struct *t, int on_rq, int running)
{
	unsigned long 		flags;
	cedf_domain_t*	 	cedf = task_cedf(t);
	cpu_entry_t* 		entry;

	BUG_ON(!cedf);

	spin_lock_irqsave(&cedf->slock, flags);
	if (running) {
		entry = &per_cpu(cedf_cpu_entries, task_cpu(t));
		BUG_ON(!entry);
		BUG_ON(entry->scheduled);
		entry->scheduled = t;
		t->rt_param.scheduled_on = task_cpu(t);
	} else
		t->rt_param.scheduled_on = NO_CPU;
	t->rt_param.linked_on = NO_CPU;

	/* setup job params */
	release_at(t, litmus_clock());

	cedf_job_arrival(t);
	spin_unlock_irqrestore(&cedf->slock, flags);
}


static void cedf_task_wake_up(struct task_struct *task)
{
	unsigned long		flags;
	cedf_domain_t*	 	cedf;
	lt_t 			now;

	BUG_ON(!task);

	cedf = task_cedf(task);
	BUG_ON(!cedf);

	spin_lock_irqsave(&cedf->slock, flags);
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
			sched_trace_job_release(task);
		}
		else if (task->time_slice)
			/* came back in time before deadline
			 */
			set_rt_flags(task, RT_F_RUNNING);
	}
	cedf_job_arrival(task);
	spin_unlock_irqrestore(&cedf->slock, flags);
}


static void cedf_task_block(struct task_struct *t)
{
	unsigned long flags;

	BUG_ON(!t);

	/* unlink if necessary */
	spin_lock_irqsave(&task_cedf(t)->slock, flags);
	unlink(t);
	spin_unlock_irqrestore(&task_cedf(t)->slock, flags);

	BUG_ON(!is_realtime(t));
}

static void cedf_task_exit(struct task_struct * t)
{
	unsigned long flags;

	BUG_ON(!t);

	/* unlink if necessary */
	spin_lock_irqsave(&task_cedf(t)->slock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		cedf_cpu_entries_array[tsk_rt(t)->scheduled_on]->
			scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	spin_unlock_irqrestore(&task_cedf(t)->slock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}

static long cedf_admit_task(struct task_struct* tsk)
{
	return (task_cpu(tsk) >= task_cedf(tsk)->first_cpu &&
	        task_cpu(tsk) <= task_cedf(tsk)->last_cpu) ? 0 : -EINVAL;
}


/*	Plugin object	*/
static struct sched_plugin cedf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "C-EDF",
	.finish_switch		= cedf_finish_switch,
	.tick			= cedf_tick,
	.task_new		= cedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= cedf_task_exit,
	.schedule		= cedf_schedule,
	.task_wake_up		= cedf_task_wake_up,
	.task_block		= cedf_task_block,
	.admit_task		= cedf_admit_task
};

static void cedf_domain_init(int first_cpu, int last_cpu)
{
	int cpu;

	/* Create new domain for this cluster. */
	cedf_domain_t *new_cedf_domain = kmalloc(sizeof(cedf_domain_t),
						     GFP_KERNEL);

	/* Initialize cluster domain. */
	edf_domain_init(&new_cedf_domain->domain, NULL,
			cedf_release_jobs);
	new_cedf_domain->first_cpu	= first_cpu;
	new_cedf_domain->last_cpu	= last_cpu;
	INIT_LIST_HEAD(&new_cedf_domain->cedf_cpu_queue);

	/* Assign all cpus in cluster to point to this domain. */
	for (cpu = first_cpu; cpu <= last_cpu; cpu++) {
		remote_cedf(cpu) = new_cedf_domain;
		cedf_domains_array[cpu] = new_cedf_domain;
	}
}

static int __init init_cedf(void)
{
	int cpu;
	cpu_entry_t *entry;

	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(cedf_cpu_entries, cpu);
		cedf_cpu_entries_array[cpu] = entry;
		atomic_set(&entry->will_schedule, 0);
		entry->linked    = NULL;
		entry->scheduled = NULL;
		entry->cpu 	 = cpu;
		INIT_LIST_HEAD(&entry->list);
	}

	/* initialize all cluster domains */
	for (cpu = 0; cpu < NR_CPUS; cpu += cluster_size)
		cedf_domain_init(cpu, cpu+cluster_size-1);

	return register_sched_plugin(&cedf_plugin);
}

module_init(init_cedf);

