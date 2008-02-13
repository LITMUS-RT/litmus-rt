/*
 * kernel/sched_gsn_edf.c
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
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>

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
 *                                be removed from the T->rt_list. It is safe to
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
	struct list_head	list;
	atomic_t		will_schedule;	/* prevent unneeded IPIs */
} cpu_entry_t;
DEFINE_PER_CPU(cpu_entry_t, gsnedf_cpu_entries);

#define set_will_schedule() \
	(atomic_set(&__get_cpu_var(gsnedf_cpu_entries).will_schedule, 1))
#define clear_will_schedule() \
	(atomic_set(&__get_cpu_var(gsnedf_cpu_entries).will_schedule, 0))
#define test_will_schedule(cpu) \
	(atomic_read(&per_cpu(gsnedf_cpu_entries, cpu).will_schedule))


#define NO_CPU 0xffffffff

/* The gsnedf_lock is used to serialize all scheduling events.
 * It protects
 */
static DEFINE_SPINLOCK(gsnedf_lock);
/* the cpus queue themselves according to priority in here */
static LIST_HEAD(gsnedf_cpu_queue);

static rt_domain_t gsnedf;


/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold gsnedf lock.
 *
 *						This really should be a heap.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	cpu_entry_t *other;
	struct list_head *pos;

	if (likely(in_list(&entry->list)))
		list_del(&entry->list);
	/* if we do not execute real-time jobs we just move
	 * to the end of the queue
	 */
	if (entry->linked) {
		list_for_each(pos, &gsnedf_cpu_queue) {
			other = list_entry(pos, cpu_entry_t, list);
			if (edf_higher_prio(entry->linked, other->linked)) {
				__list_add(&entry->list, pos->prev, pos);
				return;
			}
		}
	}
	/* if we get this far we have the lowest priority job */
	list_add_tail(&entry->list, &gsnedf_cpu_queue);
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
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold gsnedf_lock.
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
		entry = &per_cpu(gsnedf_cpu_entries, t->rt_param.linked_on);
		t->rt_param.linked_on = NO_CPU;
		link_task_to_cpu(NULL, entry);
	} else if (in_list(&t->rt_list)) {
		/* This is an interesting situation: t is scheduled,
		 * but was just recently unlinked.  It cannot be
		 * linked anywhere else (because then it would have
		 * been relinked to this CPU), thus it must be in some
		 * queue. We must remove it from the list in this
		 * case.
		 */
		list_del(&t->rt_list);
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

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold gsnedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	BUG_ON(!task);
	/* sanity check rt_list before insertion */
	BUG_ON(in_list(&task->rt_list));

	if (get_rt_flags(task) == RT_F_SLEEP) {
		/* this task has expired
		 * _schedule has already taken care of updating
		 * the release and
		 * deadline. We just must check if it has been released.
		 */
		if (is_released(task, sched_clock()))
			__add_ready(&gsnedf, task);
		else {
			/* it has got to wait */
			__add_release(&gsnedf, task);
		}

	} else
		/* this is a forced preemption
		 * thus the task stays in the ready_queue
		 * we only must make it available to others
		 */
		__add_ready(&gsnedf, task);
}

/* gsnedf_job_arrival: task is either resumed or released */
static noinline void gsnedf_job_arrival(struct task_struct* task)
{
	cpu_entry_t* last;

	BUG_ON(list_empty(&gsnedf_cpu_queue));
	BUG_ON(!task);

	/* first queue arriving job */
	requeue(task);

	/* then check for any necessary preemptions */
	last = list_entry(gsnedf_cpu_queue.prev, cpu_entry_t, list);
	if (edf_preemption_needed(&gsnedf, last->linked)) {
		/* preemption necessary */
		task = __take_ready(&gsnedf);
		TRACE("job_arrival: task %d linked to %d\n",
		      task->pid, last->cpu);
		if (last->linked)
			requeue(last->linked);

		link_task_to_cpu(task, last);
		preempt(last);
	}
}

/* check for current job releases */
static noinline  void gsnedf_release_jobs(void)
{
	struct list_head *pos, *save;
	struct task_struct   *queued;
	lt_t now = sched_clock();


	list_for_each_safe(pos, save, &gsnedf.release_queue) {
		queued = list_entry(pos, struct task_struct, rt_list);
		if (likely(is_released(queued, now))) {
			/* this one is ready to go*/
			list_del(pos);
			set_rt_flags(queued, RT_F_RUNNING);

			sched_trace_job_release(queued);
			gsnedf_job_arrival(queued);
		}
		else
			/* the release queue is ordered */
			break;
	}
}

/* gsnedf_tick - this function is called for every local timer
 *                         interrupt.
 *
 *                   checks whether the current task has expired and checks
 *                   whether we need to preempt it if it has not expired
 */
static void gsnedf_tick(struct task_struct* t)
{
	unsigned long 		flags;

	if (is_realtime(t) && budget_exhausted(t)) {
		if (!is_np(t)) {
			/* np tasks will be preempted when they become
			 * preemptable again
			 */
			set_tsk_need_resched(t);
			set_will_schedule();
			TRACE("gsnedf_scheduler_tick: "
			      "%d is preemptable "
			      " => FORCE_RESCHED\n", t->pid);
		} else {
			TRACE("gsnedf_scheduler_tick: "
			      "%d is non-preemptable, "
			      "preemption delayed.\n", t->pid);
			request_exit_np(t);
		}
	}

	/* only the first CPU needs to release jobs */
	/* FIXME: drive this from a hrtimer */
	if (smp_processor_id() == 0) {
		spin_lock_irqsave(&gsnedf_lock, flags);

		/* Try to release pending jobs */
		gsnedf_release_jobs();

		/* We don't need to check linked != scheduled since
		 * set_tsk_need_resched has been set by preempt() if necessary.
		 */

		spin_unlock_irqrestore(&gsnedf_lock, flags);
	}
}

/* caller holds gsnedf_lock */
static noinline void job_completion(struct task_struct *t)
{
	BUG_ON(!t);

	sched_trace_job_completion(t);

	TRACE_TASK(t, "job_completion().\n");

	/* set flags */
	set_rt_flags(t, RT_F_SLEEP);
	/* prepare for next period */
	edf_prepare_for_next_period(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_running(t))
		gsnedf_job_arrival(t);
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
	cpu_entry_t* 	entry = &__get_cpu_var(gsnedf_cpu_entries);
	int 			out_of_time, sleep, preempt, np, exists, blocks;
	struct task_struct* next = NULL;

	/* Will be released in finish_switch. */
	spin_lock(&gsnedf_lock);
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
	 * this.
	 */
	if (!np && (out_of_time || sleep))
		job_completion(entry->scheduled);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(&gsnedf), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked different from scheduled select linked as next.
	 */
	if ((!np || blocks) &&
	    entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked)
			next = entry->linked;
	} else
		/* Only override Linux scheduler if we have real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	spin_unlock(&gsnedf_lock);

	/* don't race with a concurrent switch */
	if (next && prev != next)
		while (next->rt_param.scheduled_on != NO_CPU)
			cpu_relax();
	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void gsnedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = &__get_cpu_var(gsnedf_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;

	prev->rt_param.scheduled_on    = NO_CPU;
	current->rt_param.scheduled_on = smp_processor_id();
}


/*	Prepare a task for running in RT mode
 */
static void gsnedf_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;

	TRACE("gsn edf: task new %d\n", t->pid);

	spin_lock_irqsave(&gsnedf_lock, flags);
	if (running) {
		entry = &per_cpu(gsnedf_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);
		entry->scheduled = t;
		t->rt_param.scheduled_on = task_cpu(t);
	} else
		t->rt_param.scheduled_on = NO_CPU;
	t->rt_param.linked_on          = NO_CPU;

	/* setup job params */
	edf_release_at(t, sched_clock());

	gsnedf_job_arrival(t);
	spin_unlock_irqrestore(&gsnedf_lock, flags);
}

static void gsnedf_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;

	spin_lock_irqsave(&gsnedf_lock, flags);
	/* We need to take suspensions because of semaphores into
	 * account! If a job resumes after being suspended due to acquiring
	 * a semaphore, it should never be treated as a new job release.
	 */
	if (get_rt_flags(task) == RT_F_EXIT_SEM) {
		set_rt_flags(task, RT_F_RUNNING);
	} else {
		now = sched_clock();
		if (is_tardy(task, now)) {
			/* new sporadic release */
			edf_release_at(task, now);
			sched_trace_job_release(task);
		}
		else if (task->time_slice)
			/* came back in time before deadline
			 */
			set_rt_flags(task, RT_F_RUNNING);
	}
	gsnedf_job_arrival(task);
	spin_unlock_irqrestore(&gsnedf_lock, flags);
}

static void gsnedf_task_block(struct task_struct *t)
{
	unsigned long flags;

	/* unlink if necessary */
	spin_lock_irqsave(&gsnedf_lock, flags);
	unlink(t);
	spin_unlock_irqrestore(&gsnedf_lock, flags);

	BUG_ON(!is_realtime(t));
	BUG_ON(t->rt_list.next != LIST_POISON1);
	BUG_ON(t->rt_list.prev != LIST_POISON2);
}


static void gsnedf_task_exit(struct task_struct * t)
{
	unsigned long flags;

	/* unlink if necessary */
	spin_lock_irqsave(&gsnedf_lock, flags);
	unlink(t);
	spin_unlock_irqrestore(&gsnedf_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
	BUG_ON(t->rt_list.next != LIST_POISON1);
	BUG_ON(t->rt_list.prev != LIST_POISON2);
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
		TRACE_TASK(new_waiter, " boosts priority\n");
		/* called with IRQs disabled */
		spin_lock(&gsnedf_lock);
		/* store new highest-priority task */
		sem->hp.task = new_waiter;
		if (sem->holder) {
			/* let holder inherit */
			sem->holder->rt_param.inh_task = new_waiter;
			unlink(sem->holder);
			gsnedf_job_arrival(sem->holder);
		}
		spin_unlock(&gsnedf_lock);
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
		spin_lock(&gsnedf_lock);

		/* Reset inh_task to NULL. */
		t->rt_param.inh_task = NULL;

		/* Check if rescheduling is necessary */
		unlink(t);
		gsnedf_job_arrival(t);
		spin_unlock(&gsnedf_lock);
	}

	return ret;
}

static long gsnedf_admit_task(struct task_struct* tsk)
{
	return 0;
}


/*	Plugin object	*/
static struct sched_plugin gsn_edf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "GSN-EDF",
	.finish_switch		= gsnedf_finish_switch,
	.tick			= gsnedf_tick,
	.task_new		= gsnedf_task_new,
	.complete_job		= edf_complete_job,
	.task_exit		= gsnedf_task_exit,
	.schedule		= gsnedf_schedule,
	.task_wake_up		= gsnedf_task_wake_up,
	.task_block		= gsnedf_task_block,
	.pi_block		= gsnedf_pi_block,
	.inherit_priority	= gsnedf_inherit_priority,
	.return_priority	= gsnedf_return_priority,
	.admit_task		= gsnedf_admit_task
};


static int __init init_gsn_edf(void)
{
	int cpu;
	cpu_entry_t *entry;

	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		entry = &per_cpu(gsnedf_cpu_entries, cpu);
		atomic_set(&entry->will_schedule, 0);
		entry->linked    = NULL;
		entry->scheduled = NULL;
		entry->cpu 	 = cpu;
		INIT_LIST_HEAD(&entry->list);
	}

	edf_domain_init(&gsnedf, NULL);
	return register_sched_plugin(&gsn_edf_plugin);
}


module_init(init_gsn_edf);
