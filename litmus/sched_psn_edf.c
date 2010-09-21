/*
 * kernel/sched_psn_edf.c
 *
 * Implementation of the PSN-EDF scheduler plugin.
 * Based on kern/sched_part_edf.c and kern/sched_gsn_edf.c.
 *
 * Suspensions and non-preemptable sections are supported.
 * Priority inheritance is not supported.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>


typedef struct {
	rt_domain_t 		domain;
	int          		cpu;
	struct task_struct* 	scheduled; /* only RT tasks */
/*
 * scheduling lock slock
 * protects the domain and serializes scheduling decisions
 */
#define slock domain.ready_lock

} psnedf_domain_t;

DEFINE_PER_CPU(psnedf_domain_t, psnedf_domains);

#define local_edf		(&__get_cpu_var(psnedf_domains).domain)
#define local_pedf		(&__get_cpu_var(psnedf_domains))
#define remote_edf(cpu)		(&per_cpu(psnedf_domains, cpu).domain)
#define remote_pedf(cpu)	(&per_cpu(psnedf_domains, cpu))
#define task_edf(task)		remote_edf(get_partition(task))
#define task_pedf(task)		remote_pedf(get_partition(task))


static void psnedf_domain_init(psnedf_domain_t* pedf,
			       check_resched_needed_t check,
			       release_jobs_t release,
			       int cpu)
{
	edf_domain_init(&pedf->domain, check, release);
	pedf->cpu      		= cpu;
	pedf->scheduled		= NULL;
}

static void requeue(struct task_struct* t, rt_domain_t *edf)
{
	if (t->state != TASK_RUNNING)
		TRACE_TASK(t, "requeue: !TASK_RUNNING\n");

	set_rt_flags(t, RT_F_RUNNING);
	if (is_released(t, litmus_clock()))
		__add_ready(edf, t);
	else
		add_release(edf, t); /* it has got to wait */
}

/* we assume the lock is being held */
static void preempt(psnedf_domain_t *pedf)
{
	preempt_if_preemptable(pedf->scheduled, pedf->cpu);
}

/* This check is trivial in partioned systems as we only have to consider
 * the CPU of the partition.
 */
static int psnedf_check_resched(rt_domain_t *edf)
{
	psnedf_domain_t *pedf = container_of(edf, psnedf_domain_t, domain);

	/* because this is a callback from rt_domain_t we already hold
	 * the necessary lock for the ready queue
	 */
	if (edf_preemption_needed(edf, pedf->scheduled)) {
		preempt(pedf);
		return 1;
	} else
		return 0;
}

static void job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t,forced);
	TRACE_TASK(t, "job_completion().\n");

	set_rt_flags(t, RT_F_SLEEP);
	prepare_for_next_period(t);
}

static void psnedf_tick(struct task_struct *t)
{
	psnedf_domain_t *pedf = local_pedf;

	/* Check for inconsistency. We don't need the lock for this since
	 * ->scheduled is only changed in schedule, which obviously is not
	 *  executing in parallel on this CPU
	 */
	BUG_ON(is_realtime(t) && t != pedf->scheduled);

	if (is_realtime(t) && budget_enforced(t) && budget_exhausted(t)) {
		if (!is_np(t)) {
			set_tsk_need_resched(t);
			TRACE("psnedf_scheduler_tick: "
			      "%d is preemptable "
			      " => FORCE_RESCHED\n", t->pid);
		} else if (is_user_np(t)) {
			TRACE("psnedf_scheduler_tick: "
			      "%d is non-preemptable, "
			      "preemption delayed.\n", t->pid);
			request_exit_np(t);
		}
	}
}

static struct task_struct* psnedf_schedule(struct task_struct * prev)
{
	psnedf_domain_t* 	pedf = local_pedf;
	rt_domain_t*		edf  = &pedf->domain;
	struct task_struct*	next;

	int 			out_of_time, sleep, preempt,
				np, exists, blocks, resched;

	raw_spin_lock(&pedf->slock);

	/* sanity checking
	 * differently from gedf, when a task exits (dead)
	 * pedf->schedule may be null and prev _is_ realtime
	 */
	BUG_ON(pedf->scheduled && pedf->scheduled != prev);
	BUG_ON(pedf->scheduled && !is_realtime(prev));

	/* (0) Determine state */
	exists      = pedf->scheduled != NULL;
	blocks      = exists && !is_running(pedf->scheduled);
	out_of_time = exists &&
				  budget_enforced(pedf->scheduled) &&
				  budget_exhausted(pedf->scheduled);
	np 	    = exists && is_np(pedf->scheduled);
	sleep	    = exists && get_rt_flags(pedf->scheduled) == RT_F_SLEEP;
	preempt     = edf_preemption_needed(edf, prev);

	/* If we need to preempt do so.
	 * The following checks set resched to 1 in case of special
	 * circumstances.
	 */
	resched = preempt;

	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		resched = 1;

	/* Request a sys_exit_np() call if we would like to preempt but cannot.
	 * Multiple calls to request_exit_np() don't hurt.
	 */
	if (np && (out_of_time || preempt || sleep))
		request_exit_np(pedf->scheduled);

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this.
	 */
	if (!np && (out_of_time || sleep) && !blocks) {
		job_completion(pedf->scheduled, !sleep);
		resched = 1;
	}

	/* The final scheduling decision. Do we need to switch for some reason?
	 * Switch if we are in RT mode and have no task or if we need to
	 * resched.
	 */
	next = NULL;
	if ((!np || blocks) && (resched || !exists)) {
		/* When preempting a task that does not block, then
		 * re-insert it into either the ready queue or the
		 * release queue (if it completed). requeue() picks
		 * the appropriate queue.
		 */
		if (pedf->scheduled && !blocks)
			requeue(pedf->scheduled, edf);
		next = __take_ready(edf);
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	if (next) {
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
		set_rt_flags(next, RT_F_RUNNING);
	} else {
		TRACE("becoming idle at %llu\n", litmus_clock());
	}

	pedf->scheduled = next;
	raw_spin_unlock(&pedf->slock);

	return next;
}


/*	Prepare a task for running in RT mode
 */
static void psnedf_task_new(struct task_struct * t, int on_rq, int running)
{
	rt_domain_t* 		edf  = task_edf(t);
	psnedf_domain_t* 	pedf = task_pedf(t);
	unsigned long		flags;

	TRACE_TASK(t, "psn edf: task new, cpu = %d\n",
		   t->rt_param.task_params.cpu);

	/* setup job parameters */
	release_at(t, litmus_clock());

	/* The task should be running in the queue, otherwise signal
	 * code will try to wake it up with fatal consequences.
	 */
	raw_spin_lock_irqsave(&pedf->slock, flags);
	if (running) {
		/* there shouldn't be anything else running at the time */
		BUG_ON(pedf->scheduled);
		pedf->scheduled = t;
	} else {
		requeue(t, edf);
		/* maybe we have to reschedule */
		preempt(pedf);
	}
	raw_spin_unlock_irqrestore(&pedf->slock, flags);
}

static void psnedf_task_wake_up(struct task_struct *task)
{
	unsigned long		flags;
	psnedf_domain_t* 	pedf = task_pedf(task);
	rt_domain_t* 		edf  = task_edf(task);
	lt_t			now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());
	raw_spin_lock_irqsave(&pedf->slock, flags);
	BUG_ON(is_queued(task));
	/* We need to take suspensions because of semaphores into
	 * account! If a job resumes after being suspended due to acquiring
	 * a semaphore, it should never be treated as a new job release.
	 *
	 * FIXME: This should be done in some more predictable and userspace-controlled way.
	 */
	now = litmus_clock();
	if (is_tardy(task, now) &&
	    get_rt_flags(task) != RT_F_EXIT_SEM) {
		/* new sporadic release */
		release_at(task, now);
		sched_trace_task_release(task);
	}

	/* Only add to ready queue if it is not the currently-scheduled
	 * task. This could be the case if a task was woken up concurrently
	 * on a remote CPU before the executing CPU got around to actually
	 * de-scheduling the task, i.e., wake_up() raced with schedule()
	 * and won.
	 */
	if (pedf->scheduled != task)
		requeue(task, edf);

	raw_spin_unlock_irqrestore(&pedf->slock, flags);
	TRACE_TASK(task, "wake up done\n");
}

static void psnedf_task_block(struct task_struct *t)
{
	/* only running tasks can block, thus t is in no queue */
	TRACE_TASK(t, "block at %llu, state=%d\n", litmus_clock(), t->state);

	BUG_ON(!is_realtime(t));
	BUG_ON(is_queued(t));
}

static void psnedf_task_exit(struct task_struct * t)
{
	unsigned long flags;
	psnedf_domain_t* 	pedf = task_pedf(t);
	rt_domain_t*		edf;

	raw_spin_lock_irqsave(&pedf->slock, flags);
	if (is_queued(t)) {
		/* dequeue */
		edf  = task_edf(t);
		remove(edf, t);
	}
	if (pedf->scheduled == t)
		pedf->scheduled = NULL;

	TRACE_TASK(t, "RIP, now reschedule\n");

	preempt(pedf);
	raw_spin_unlock_irqrestore(&pedf->slock, flags);
}

#ifdef CONFIG_FMLP
static long psnedf_pi_block(struct pi_semaphore *sem,
			    struct task_struct *new_waiter)
{
	psnedf_domain_t* 	pedf;
	rt_domain_t*		edf;
	struct task_struct*	t;
	int cpu  = get_partition(new_waiter);

	BUG_ON(!new_waiter);

	if (edf_higher_prio(new_waiter, sem->hp.cpu_task[cpu])) {
		TRACE_TASK(new_waiter, " boosts priority\n");
		pedf = task_pedf(new_waiter);
		edf  = task_edf(new_waiter);

		/* interrupts already disabled */
		raw_spin_lock(&pedf->slock);

		/* store new highest-priority task */
		sem->hp.cpu_task[cpu] = new_waiter;
		if (sem->holder &&
		    get_partition(sem->holder) == get_partition(new_waiter)) {
			/* let holder inherit */
			sem->holder->rt_param.inh_task = new_waiter;
			t = sem->holder;
			if (is_queued(t)) {
				/* queued in domain*/
				remove(edf, t);
				/* readd to make priority change take place */
				/* FIXME: this looks outdated */
				if (is_released(t, litmus_clock()))
					__add_ready(edf, t);
				else
					add_release(edf, t);
			}
		}

		/* check if we need to reschedule */
		if (edf_preemption_needed(edf, current))
			preempt(pedf);

		raw_spin_unlock(&pedf->slock);
	}

	return 0;
}

static long psnedf_inherit_priority(struct pi_semaphore *sem,
				    struct task_struct *new_owner)
{
	int cpu  = get_partition(new_owner);

	new_owner->rt_param.inh_task = sem->hp.cpu_task[cpu];
	if (sem->hp.cpu_task[cpu] && new_owner != sem->hp.cpu_task[cpu]) {
		TRACE_TASK(new_owner,
			   "inherited priority from %s/%d\n",
			   sem->hp.cpu_task[cpu]->comm,
			   sem->hp.cpu_task[cpu]->pid);
	} else
		TRACE_TASK(new_owner,
			   "cannot inherit priority: "
			   "no higher priority job waits on this CPU!\n");
	/* make new owner non-preemptable as required by FMLP under
	 * PSN-EDF.
	 */
	make_np(new_owner);
	return 0;
}


/* This function is called on a semaphore release, and assumes that
 * the current task is also the semaphore holder.
 */
static long psnedf_return_priority(struct pi_semaphore *sem)
{
	struct task_struct* 	t    = current;
	psnedf_domain_t* 	pedf = task_pedf(t);
	rt_domain_t*		edf  = task_edf(t);
	int 			ret  = 0;
	int			cpu  = get_partition(current);
	int still_np;


        /* Find new highest-priority semaphore task
	 * if holder task is the current hp.cpu_task[cpu].
	 *
	 * Calling function holds sem->wait.lock.
	 */
	if (t == sem->hp.cpu_task[cpu])
		edf_set_hp_cpu_task(sem, cpu);

	still_np = take_np(current);

	/* Since we don't nest resources, this
	 * should always be zero */
	BUG_ON(still_np);

	if (current->rt_param.inh_task) {
		TRACE_CUR("return priority of %s/%d\n",
			  current->rt_param.inh_task->comm,
			  current->rt_param.inh_task->pid);
	} else
		TRACE_CUR(" no priority to return %p\n", sem);


	/* Always check for delayed preemptions that might have become
	 * necessary due to non-preemptive execution.
	 */
	raw_spin_lock(&pedf->slock);

	/* Reset inh_task to NULL. */
	current->rt_param.inh_task = NULL;

	/* check if we need to reschedule */
	if (edf_preemption_needed(edf, current))
		preempt(pedf);

	raw_spin_unlock(&pedf->slock);


	return ret;
}

#endif

static long psnedf_admit_task(struct task_struct* tsk)
{
	return task_cpu(tsk) == tsk->rt_param.task_params.cpu ? 0 : -EINVAL;
}

/*	Plugin object	*/
static struct sched_plugin psn_edf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "PSN-EDF",
#ifdef CONFIG_SRP
	.srp_active		= 1,
#endif
	.tick			= psnedf_tick,
	.task_new		= psnedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= psnedf_task_exit,
	.schedule		= psnedf_schedule,
	.task_wake_up		= psnedf_task_wake_up,
	.task_block		= psnedf_task_block,
#ifdef CONFIG_FMLP
	.fmlp_active		= 1,
	.pi_block		= psnedf_pi_block,
	.inherit_priority	= psnedf_inherit_priority,
	.return_priority	= psnedf_return_priority,
#endif
	.admit_task		= psnedf_admit_task
};


static int __init init_psn_edf(void)
{
	int i;

	/* We do not really want to support cpu hotplug, do we? ;)
	 * However, if we are so crazy to do so,
	 * we cannot use num_online_cpu()
	 */
	for (i = 0; i < num_online_cpus(); i++) {
		psnedf_domain_init(remote_pedf(i),
				   psnedf_check_resched,
				   NULL, i);
	}
	return register_sched_plugin(&psn_edf_plugin);
}

module_init(init_psn_edf);

