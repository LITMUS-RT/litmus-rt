/*
 * litmus/sched_gsn_edf.c
 *
 * Implementation of the GSN-EDF scheduling algorithm.
 *
 * This version uses the simple approach and serializes all scheduling
 * decisions by the use of a queue lock. This is probably not the
 * best way to do it, but it should suffice for now.
 */

#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>

#include <litmus/preempt.h>

#include <litmus/bheap.h>

#include <linux/module.h>

/* Overview of GSN-EDF operations.
 *
 * For a detailed explanation of GSN-EDF have a look at the FMLP paper. This
 * description only covers how the individual operations are implemented in
 * LITMUS.
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
 *                                currently queued in the gsnedf queue it will
 *                                be removed from the rt_domain. It is safe to
 *                                call unlink(T) if T is not linked. T may not
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
 *                                no/etc. has to be updated before requeu(T) is
 *                                called. It is not safe to call requeue(T)
 *                                when T is already queued. T may not be NULL.
 *
 * gsnedf_job_arrival(T)	- This is the catch all function when T enters
 *                                the system after either a suspension or at a
 *                                job release. It will queue T (which means it
 *                                is not safe to call gsnedf_job_arrival(T) if
 *                                T is already queued) and then check whether a
 *                                preemption is necessary. If a preemption is
 *                                necessary it will update the linkage
 *                                accordingly and cause scheduled to be called
 *                                (either with an IPI or need_resched). It is
 *                                safe to call gsnedf_job_arrival(T) if T's
 *                                next job has not been actually released yet
 *                                (releast time in the future). T will be put
 *                                on the release queue in that case.
 *
 * job_completion(T)		- Take care of everything that needs to be done
 *                                to prepare T for its next release and place
 *                                it in the right queue with
 *                                gsnedf_job_arrival().
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
	struct bheap_node*	hn;
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, gsnedf_cpu_entries);

cpu_entry_t* gsnedf_cpus[NR_CPUS];

/* the cpus queue themselves according to priority in here */
static struct bheap_node gsnedf_heap_node[NR_CPUS];
static struct bheap      gsnedf_cpu_heap;

static rt_domain_t gsnedf;
#define gsnedf_lock (gsnedf.ready_lock)


/* Uncomment this if you want to see all scheduling decisions in the
 * TRACE() log.
#define WANT_ALL_SCHED_EVENTS
 */

static int cpu_lower_prio(struct bheap_node *_a, struct bheap_node *_b)
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
 *                       order in the cpu queue. Caller must hold gsnedf lock.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	if (likely(bheap_node_in_heap(entry->hn)))
		bheap_delete(cpu_lower_prio, &gsnedf_cpu_heap, entry->hn);
	bheap_insert(cpu_lower_prio, &gsnedf_cpu_heap, entry->hn);
}

/* caller must hold gsnedf lock */
static cpu_entry_t* lowest_prio_cpu(void)
{
	struct bheap_node* hn;
	hn = bheap_peek(cpu_lower_prio, &gsnedf_cpu_heap);
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
			sched = &per_cpu(gsnedf_cpu_entries, on_cpu);
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
#ifdef WANT_ALL_SCHED_EVENTS
	if (linked)
		TRACE_TASK(linked, "linked to %d.\n", entry->cpu);
	else
		TRACE("NULL linked to %d.\n", entry->cpu);
#endif
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold gsnedf_lock.
 */
static noinline void unlink(struct task_struct* t)
{
    	cpu_entry_t *entry;

	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(gsnedf_cpu_entries, t->rt_param.linked_on);
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
		remove(&gsnedf, t);
	}
}


/* preempt - force a CPU to reschedule
 */
static void preempt(cpu_entry_t *entry)
{
	preempt_if_preemptable(entry->scheduled, entry->cpu);
}

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold gsnedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	BUG_ON(!task);
	/* sanity check before insertion */
	BUG_ON(is_queued(task));

	if (is_released(task, litmus_clock()))
		__add_ready(&gsnedf, task);
	else {
		/* it has got to wait */
		add_release(&gsnedf, task);
	}
}

/* check for any necessary preemptions */
static void check_for_preemptions(void)
{
	struct task_struct *task;
	cpu_entry_t* last;

	for(last = lowest_prio_cpu();
	    edf_preemption_needed(&gsnedf, last->linked);
	    last = lowest_prio_cpu()) {
		/* preemption necessary */
		task = __take_ready(&gsnedf);
		TRACE("check_for_preemptions: attempting to link task %d to %d\n",
		      task->pid, last->cpu);
		if (last->linked)
			requeue(last->linked);
		link_task_to_cpu(task, last);
		preempt(last);
	}
}

/* gsnedf_job_arrival: task is either resumed or released */
static noinline void gsnedf_job_arrival(struct task_struct* task)
{
	BUG_ON(!task);

	requeue(task);
	check_for_preemptions();
}

static void gsnedf_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&gsnedf_lock, flags);

	__merge_ready(rt, tasks);
	check_for_preemptions();

	raw_spin_unlock_irqrestore(&gsnedf_lock, flags);
}

/* caller holds gsnedf_lock */
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
		gsnedf_job_arrival(t);
}

/* gsnedf_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void gsnedf_tick(struct task_struct* t)
{
	if (is_realtime(t) && budget_enforced(t) && budget_exhausted(t)) {
		if (!is_np(t)) {
			/* np tasks will be preempted when they become
			 * preemptable again
			 */
			litmus_reschedule_local();
			TRACE("gsnedf_scheduler_tick: "
			      "%d is preemptable "
			      " => FORCE_RESCHED\n", t->pid);
		} else if (is_user_np(t)) {
			TRACE("gsnedf_scheduler_tick: "
			      "%d is non-preemptable, "
			      "preemption delayed.\n", t->pid);
			request_exit_np(t);
		}
	}
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
static struct task_struct* gsnedf_schedule(struct task_struct * prev)
{
	cpu_entry_t* entry = &__get_cpu_var(gsnedf_cpu_entries);
	int out_of_time, sleep, preempt, np, exists, blocks;
	struct task_struct* next = NULL;

#ifdef CONFIG_RELEASE_MASTER
	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (gsnedf.release_master == entry->cpu)
		return NULL;
#endif

	raw_spin_lock(&gsnedf_lock);

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);

	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists &&
				  budget_enforced(entry->scheduled) &&
				  budget_exhausted(entry->scheduled);
	np 	    = exists && is_np(entry->scheduled);
	sleep	    = exists && get_rt_flags(entry->scheduled) == RT_F_SLEEP;
	preempt     = entry->scheduled != entry->linked;

#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "invoked gsnedf_schedule.\n");
#endif

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d np:%d sleep:%d preempt:%d "
			   "state:%d sig:%d\n",
			   blocks, out_of_time, np, sleep, preempt,
			   prev->state, signal_pending(prev));
	if (entry->linked && preempt)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);


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
	 * this. Don't do a job completion if we block (can't have timers running
	 * for blocked jobs). Preemption go first for the same reason.
	 */
	if (!np && (out_of_time || sleep) && !blocks && !preempt)
		job_completion(entry->scheduled, !sleep);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(&gsnedf), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
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
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	sched_state_task_picked();

	raw_spin_unlock(&gsnedf_lock);

#ifdef WANT_ALL_SCHED_EVENTS
	TRACE("gsnedf_lock released, next=0x%p\n", next);

	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());
#endif


	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void gsnedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = &__get_cpu_var(gsnedf_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "switched away from\n");
#endif
}


/*	Prepare a task for running in RT mode
 */
static void gsnedf_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;

	TRACE("gsn edf: task new %d\n", t->pid);

	raw_spin_lock_irqsave(&gsnedf_lock, flags);

	/* setup job params */
	release_at(t, litmus_clock());

	if (running) {
		entry = &per_cpu(gsnedf_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);

#ifdef CONFIG_RELEASE_MASTER
		if (entry->cpu != gsnedf.release_master) {
#endif
			entry->scheduled = t;
			tsk_rt(t)->scheduled_on = task_cpu(t);
#ifdef CONFIG_RELEASE_MASTER
		} else {
			/* do not schedule on release master */
			preempt(entry); /* force resched */
			tsk_rt(t)->scheduled_on = NO_CPU;
		}
#endif
	} else {
		t->rt_param.scheduled_on = NO_CPU;
	}
	t->rt_param.linked_on          = NO_CPU;

	gsnedf_job_arrival(t);
	raw_spin_unlock_irqrestore(&gsnedf_lock, flags);
}

static void gsnedf_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	raw_spin_lock_irqsave(&gsnedf_lock, flags);
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
		else {
			if (task->rt.time_slice) {
				/* came back in time before deadline
				*/
				set_rt_flags(task, RT_F_RUNNING);
			}
		}
	}
	gsnedf_job_arrival(task);
	raw_spin_unlock_irqrestore(&gsnedf_lock, flags);
}

static void gsnedf_task_block(struct task_struct *t)
{
	unsigned long flags;

	TRACE_TASK(t, "block at %llu\n", litmus_clock());

	/* unlink if necessary */
	raw_spin_lock_irqsave(&gsnedf_lock, flags);
	unlink(t);
	raw_spin_unlock_irqrestore(&gsnedf_lock, flags);

	BUG_ON(!is_realtime(t));
}


static void gsnedf_task_exit(struct task_struct * t)
{
	unsigned long flags;

	/* unlink if necessary */
	raw_spin_lock_irqsave(&gsnedf_lock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		gsnedf_cpus[tsk_rt(t)->scheduled_on]->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	raw_spin_unlock_irqrestore(&gsnedf_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}

#ifdef CONFIG_FMLP

/* Update the queue position of a task that got it's priority boosted via
 * priority inheritance. */
static void update_queue_position(struct task_struct *holder)
{
	/* We don't know whether holder is in the ready queue. It should, but
	 * on a budget overrun it may already be in a release queue.  Hence,
	 * calling unlink() is not possible since it assumes that the task is
	 * not in a release queue.  However, we can safely check whether
	 * sem->holder is currently in a queue or scheduled after locking both
	 * the release and the ready queue lock. */

	/* Assumption: caller holds gsnedf_lock */

	int check_preempt = 0;

	if (tsk_rt(holder)->linked_on != NO_CPU) {
		TRACE_TASK(holder, "%s: linked  on %d\n",
			   __FUNCTION__, tsk_rt(holder)->linked_on);
		/* Holder is scheduled; need to re-order CPUs.
		 * We can't use heap_decrease() here since
		 * the cpu_heap is ordered in reverse direction, so
		 * it is actually an increase. */
		bheap_delete(cpu_lower_prio, &gsnedf_cpu_heap,
			    gsnedf_cpus[tsk_rt(holder)->linked_on]->hn);
		bheap_insert(cpu_lower_prio, &gsnedf_cpu_heap,
			    gsnedf_cpus[tsk_rt(holder)->linked_on]->hn);
	} else {
		/* holder may be queued: first stop queue changes */
		raw_spin_lock(&gsnedf.release_lock);
		if (is_queued(holder)) {
			TRACE_TASK(holder, "%s: is queued\n",
				   __FUNCTION__);
			/* We need to update the position
			 * of holder in some heap. Note that this
			 * may be a release heap. */
			check_preempt =
				!bheap_decrease(edf_ready_order,
					       tsk_rt(holder)->heap_node);
		} else {
			/* Nothing to do: if it is not queued and not linked
			 * then it is currently being moved by other code
			 * (e.g., a timer interrupt handler) that will use the
			 * correct priority when enqueuing the task. */
			TRACE_TASK(holder, "%s: is NOT queued => Done.\n",
				   __FUNCTION__);
		}
		raw_spin_unlock(&gsnedf.release_lock);

		/* If holder was enqueued in a release heap, then the following
		 * preemption check is pointless, but we can't easily detect
		 * that case. If you want to fix this, then consider that
		 * simply adding a state flag requires O(n) time to update when
		 * releasing n tasks, which conflicts with the goal to have
		 * O(log n) merges. */
		if (check_preempt) {
			/* heap_decrease() hit the top level of the heap: make
			 * sure preemption checks get the right task, not the
			 * potentially stale cache. */
			bheap_uncache_min(edf_ready_order,
					 &gsnedf.ready_queue);
			check_for_preemptions();
		}
	}
}

static long gsnedf_pi_block(struct pi_semaphore *sem,
			    struct task_struct *new_waiter)
{
	/* This callback has to handle the situation where a new waiter is
	 * added to the wait queue of the semaphore.
	 *
	 * We must check if has a higher priority than the currently
	 * highest-priority task, and then potentially reschedule.
	 */

	BUG_ON(!new_waiter);

	if (edf_higher_prio(new_waiter, sem->hp.task)) {
		TRACE_TASK(new_waiter, " boosts priority via %p\n", sem);
		/* called with IRQs disabled */
		raw_spin_lock(&gsnedf_lock);
		/* store new highest-priority task */
		sem->hp.task = new_waiter;
		if (sem->holder) {
			TRACE_TASK(sem->holder,
				   " holds %p and will inherit from %s/%d\n",
				   sem,
				   new_waiter->comm, new_waiter->pid);
			/* let holder inherit */
			sem->holder->rt_param.inh_task = new_waiter;
			update_queue_position(sem->holder);
		}
		raw_spin_unlock(&gsnedf_lock);
	}

	return 0;
}

static long gsnedf_inherit_priority(struct pi_semaphore *sem,
				    struct task_struct *new_owner)
{
	/* We don't need to acquire the gsnedf_lock since at the time of this
	 * call new_owner isn't actually scheduled yet (it's still sleeping)
	 * and since the calling function already holds sem->wait.lock, which
	 * prevents concurrent sem->hp.task changes.
	 */

	if (sem->hp.task && sem->hp.task != new_owner) {
		new_owner->rt_param.inh_task = sem->hp.task;
		TRACE_TASK(new_owner, "inherited priority from %s/%d\n",
			   sem->hp.task->comm, sem->hp.task->pid);
	} else
		TRACE_TASK(new_owner,
			   "cannot inherit priority, "
			   "no higher priority job waits.\n");
	return 0;
}

/* This function is called on a semaphore release, and assumes that
 * the current task is also the semaphore holder.
 */
static long gsnedf_return_priority(struct pi_semaphore *sem)
{
	struct task_struct* t = current;
	int ret = 0;

        /* Find new highest-priority semaphore task
	 * if holder task is the current hp.task.
	 *
	 * Calling function holds sem->wait.lock.
	 */
	if (t == sem->hp.task)
		edf_set_hp_task(sem);

	TRACE_CUR("gsnedf_return_priority for lock %p\n", sem);

	if (t->rt_param.inh_task) {
		/* interrupts already disabled by PI code */
		raw_spin_lock(&gsnedf_lock);

		/* Reset inh_task to NULL. */
		t->rt_param.inh_task = NULL;

		/* Check if rescheduling is necessary */
		unlink(t);
		gsnedf_job_arrival(t);
		raw_spin_unlock(&gsnedf_lock);
	}

	return ret;
}

#endif

static long gsnedf_admit_task(struct task_struct* tsk)
{
	return 0;
}

static long gsnedf_activate_plugin(void)
{
	int cpu;
	cpu_entry_t *entry;

	bheap_init(&gsnedf_cpu_heap);
#ifdef CONFIG_RELEASE_MASTER
	gsnedf.release_master = atomic_read(&release_master_cpu);
#endif

	for_each_online_cpu(cpu) {
		entry = &per_cpu(gsnedf_cpu_entries, cpu);
		bheap_node_init(&entry->hn, entry);
		entry->linked    = NULL;
		entry->scheduled = NULL;
#ifdef CONFIG_RELEASE_MASTER
		if (cpu != gsnedf.release_master) {
#endif
			TRACE("GSN-EDF: Initializing CPU #%d.\n", cpu);
			update_cpu_position(entry);
#ifdef CONFIG_RELEASE_MASTER
		} else {
			TRACE("GSN-EDF: CPU %d is release master.\n", cpu);
		}
#endif
	}
	return 0;
}

/*	Plugin object	*/
static struct sched_plugin gsn_edf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "GSN-EDF",
	.finish_switch		= gsnedf_finish_switch,
	.tick			= gsnedf_tick,
	.task_new		= gsnedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= gsnedf_task_exit,
	.schedule		= gsnedf_schedule,
	.task_wake_up		= gsnedf_task_wake_up,
	.task_block		= gsnedf_task_block,
#ifdef CONFIG_FMLP
	.fmlp_active		= 1,
	.pi_block		= gsnedf_pi_block,
	.inherit_priority	= gsnedf_inherit_priority,
	.return_priority	= gsnedf_return_priority,
#endif
	.admit_task		= gsnedf_admit_task,
	.activate_plugin	= gsnedf_activate_plugin,
};


static int __init init_gsn_edf(void)
{
	int cpu;
	cpu_entry_t *entry;

	bheap_init(&gsnedf_cpu_heap);
	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(gsnedf_cpu_entries, cpu);
		gsnedf_cpus[cpu] = entry;
		entry->cpu 	 = cpu;
		entry->hn        = &gsnedf_heap_node[cpu];
		bheap_node_init(&entry->hn, entry);
	}
	edf_domain_init(&gsnedf, NULL, gsnedf_release_jobs);
	return register_sched_plugin(&gsn_edf_plugin);
}


module_init(init_gsn_edf);
