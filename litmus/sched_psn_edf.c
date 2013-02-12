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
#include <litmus/preempt.h>
#include <litmus/budget.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>
#include <litmus/trace.h>

/* to set up domain/cpu mappings */
#include <litmus/litmus_proc.h>

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

	tsk_rt(t)->completed = 0;
	if (is_early_releasing(t) || is_released(t, litmus_clock()))
		__add_ready(edf, t);
	else
		add_release(edf, t); /* it has got to wait */
}

/* we assume the lock is being held */
static void preempt(psnedf_domain_t *pedf)
{
	preempt_if_preemptable(pedf->scheduled, pedf->cpu);
}

#ifdef CONFIG_LITMUS_LOCKING

static void boost_priority(struct task_struct* t)
{
	unsigned long		flags;
	psnedf_domain_t* 	pedf = task_pedf(t);
	lt_t			now;

	raw_spin_lock_irqsave(&pedf->slock, flags);
	now = litmus_clock();

	TRACE_TASK(t, "priority boosted at %llu\n", now);

	tsk_rt(t)->priority_boosted = 1;
	tsk_rt(t)->boost_start_time = now;

	if (pedf->scheduled != t) {
		/* holder may be queued: first stop queue changes */
		raw_spin_lock(&pedf->domain.release_lock);
		if (is_queued(t) &&
		    /* If it is queued, then we need to re-order. */
		    bheap_decrease(edf_ready_order, tsk_rt(t)->heap_node) &&
		    /* If we bubbled to the top, then we need to check for preemptions. */
		    edf_preemption_needed(&pedf->domain, pedf->scheduled))
				preempt(pedf);
		raw_spin_unlock(&pedf->domain.release_lock);
	} /* else: nothing to do since the job is not queued while scheduled */

	raw_spin_unlock_irqrestore(&pedf->slock, flags);
}

static void unboost_priority(struct task_struct* t)
{
	unsigned long		flags;
	psnedf_domain_t* 	pedf = task_pedf(t);
	lt_t			now;

	raw_spin_lock_irqsave(&pedf->slock, flags);
	now = litmus_clock();

	/* Assumption: this only happens when the job is scheduled.
	 * Exception: If t transitioned to non-real-time mode, we no longer
	 * care about it. */
	BUG_ON(pedf->scheduled != t && is_realtime(t));

	TRACE_TASK(t, "priority restored at %llu\n", now);

	tsk_rt(t)->priority_boosted = 0;
	tsk_rt(t)->boost_start_time = 0;

	/* check if this changes anything */
	if (edf_preemption_needed(&pedf->domain, pedf->scheduled))
		preempt(pedf);

	raw_spin_unlock_irqrestore(&pedf->slock, flags);
}

#endif

static int psnedf_preempt_check(psnedf_domain_t *pedf)
{
	if (edf_preemption_needed(&pedf->domain, pedf->scheduled)) {
		preempt(pedf);
		return 1;
	} else
		return 0;
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
	return psnedf_preempt_check(pedf);
}

static void job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t,forced);
	TRACE_TASK(t, "job_completion().\n");

	tsk_rt(t)->completed = 0;
	prepare_for_next_period(t);
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
	sleep	    = exists && is_completed(pedf->scheduled);
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
	} else {
		TRACE("becoming idle at %llu\n", litmus_clock());
	}

	pedf->scheduled = next;
	sched_state_task_picked();
	raw_spin_unlock(&pedf->slock);

	return next;
}


/*	Prepare a task for running in RT mode
 */
static void psnedf_task_new(struct task_struct * t, int on_rq, int is_scheduled)
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
	if (is_scheduled) {
		/* there shouldn't be anything else scheduled at the time */
		BUG_ON(pedf->scheduled);
		pedf->scheduled = t;
	} else {
		/* !is_scheduled means it is not scheduled right now, but it
		 * does not mean that it is suspended. If it is not suspended,
		 * it still needs to be requeued. If it is suspended, there is
		 * nothing that we need to do as it will be handled by the
		 * wake_up() handler. */
		if (is_running(t)) {
			requeue(t, edf);
			/* maybe we have to reschedule */
			psnedf_preempt_check(pedf);
		}
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
	now = litmus_clock();
	if (is_sporadic(task) && is_tardy(task, now)
#ifdef CONFIG_LITMUS_LOCKING
	/* We need to take suspensions because of semaphores into
	 * account! If a job resumes after being suspended due to acquiring
	 * a semaphore, it should never be treated as a new job release.
	 */
	    && !is_priority_boosted(task)
#endif
		) {
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
	if (pedf->scheduled != task) {
		requeue(task, edf);
		psnedf_preempt_check(pedf);
	}

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

#ifdef CONFIG_LITMUS_LOCKING

#include <litmus/fdso.h>
#include <litmus/srp.h>

/* ******************** SRP support ************************ */

static unsigned int psnedf_get_srp_prio(struct task_struct* t)
{
	return get_rt_relative_deadline(t);
}

/* ******************** FMLP support ********************** */

/* struct for semaphore with priority inheritance */
struct fmlp_semaphore {
	struct litmus_lock litmus_lock;

	/* current resource holder */
	struct task_struct *owner;

	/* FIFO queue of waiting tasks */
	wait_queue_head_t wait;
};

static inline struct fmlp_semaphore* fmlp_from_lock(struct litmus_lock* lock)
{
	return container_of(lock, struct fmlp_semaphore, litmus_lock);
}
int psnedf_fmlp_lock(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct fmlp_semaphore *sem = fmlp_from_lock(l);
	wait_queue_t wait;
	unsigned long flags;

	if (!is_realtime(t))
		return -EPERM;

	/* prevent nested lock acquisition --- not supported by FMLP */
	if (tsk_rt(t)->num_locks_held ||
	    tsk_rt(t)->num_local_locks_held)
		return -EBUSY;

	spin_lock_irqsave(&sem->wait.lock, flags);

	if (sem->owner) {
		/* resource is not free => must suspend and wait */

		init_waitqueue_entry(&wait, t);

		/* FIXME: interruptible would be nice some day */
		set_task_state(t, TASK_UNINTERRUPTIBLE);

		__add_wait_queue_tail_exclusive(&sem->wait, &wait);

		TS_LOCK_SUSPEND;

		/* release lock before sleeping */
		spin_unlock_irqrestore(&sem->wait.lock, flags);

		/* We depend on the FIFO order.  Thus, we don't need to recheck
		 * when we wake up; we are guaranteed to have the lock since
		 * there is only one wake up per release.
		 */

		schedule();

		TS_LOCK_RESUME;

		/* Since we hold the lock, no other task will change
		 * ->owner. We can thus check it without acquiring the spin
		 * lock. */
		BUG_ON(sem->owner != t);
	} else {
		/* it's ours now */
		sem->owner = t;

		/* mark the task as priority-boosted. */
		boost_priority(t);

		spin_unlock_irqrestore(&sem->wait.lock, flags);
	}

	tsk_rt(t)->num_locks_held++;

	return 0;
}

int psnedf_fmlp_unlock(struct litmus_lock* l)
{
	struct task_struct *t = current, *next;
	struct fmlp_semaphore *sem = fmlp_from_lock(l);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&sem->wait.lock, flags);

	if (sem->owner != t) {
		err = -EINVAL;
		goto out;
	}

	tsk_rt(t)->num_locks_held--;

	/* we lose the benefit of priority boosting */

	unboost_priority(t);

	/* check if there are jobs waiting for this resource */
	next = __waitqueue_remove_first(&sem->wait);
	if (next) {
		/* boost next job */
		boost_priority(next);

		/* next becomes the resouce holder */
		sem->owner = next;

		/* wake up next */
		wake_up_process(next);
	} else
		/* resource becomes available */
		sem->owner = NULL;

out:
	spin_unlock_irqrestore(&sem->wait.lock, flags);
	return err;
}

int psnedf_fmlp_close(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct fmlp_semaphore *sem = fmlp_from_lock(l);
	unsigned long flags;

	int owner;

	spin_lock_irqsave(&sem->wait.lock, flags);

	owner = sem->owner == t;

	spin_unlock_irqrestore(&sem->wait.lock, flags);

	if (owner)
		psnedf_fmlp_unlock(l);

	return 0;
}

void psnedf_fmlp_free(struct litmus_lock* lock)
{
	kfree(fmlp_from_lock(lock));
}

static struct litmus_lock_ops psnedf_fmlp_lock_ops = {
	.close  = psnedf_fmlp_close,
	.lock   = psnedf_fmlp_lock,
	.unlock = psnedf_fmlp_unlock,
	.deallocate = psnedf_fmlp_free,
};

static struct litmus_lock* psnedf_new_fmlp(void)
{
	struct fmlp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	sem->owner   = NULL;
	init_waitqueue_head(&sem->wait);
	sem->litmus_lock.ops = &psnedf_fmlp_lock_ops;

	return &sem->litmus_lock;
}

/* **** lock constructor **** */


static long psnedf_allocate_lock(struct litmus_lock **lock, int type,
				 void* __user unused)
{
	int err = -ENXIO;
	struct srp_semaphore* srp;

	/* PSN-EDF currently supports the SRP for local resources and the FMLP
	 * for global resources. */
	switch (type) {
	case FMLP_SEM:
		/* Flexible Multiprocessor Locking Protocol */
		*lock = psnedf_new_fmlp();
		if (*lock)
			err = 0;
		else
			err = -ENOMEM;
		break;

	case SRP_SEM:
		/* Baker's Stack Resource Policy */
		srp = allocate_srp_semaphore();
		if (srp) {
			*lock = &srp->litmus_lock;
			err = 0;
		} else
			err = -ENOMEM;
		break;
	};

	return err;
}

#endif

static struct domain_proc_info psnedf_domain_proc_info;
static long psnedf_get_domain_proc_info(struct domain_proc_info **ret)
{
	*ret = &psnedf_domain_proc_info;
	return 0;
}

static void psnedf_setup_domain_proc(void)
{
	int i, cpu;
	int release_master =
#ifdef CONFIG_RELEASE_MASTER
		atomic_read(&release_master_cpu);
#else
		NO_CPU;
#endif
	int num_rt_cpus = num_online_cpus() - (release_master != NO_CPU);
	struct cd_mapping *cpu_map, *domain_map;

	memset(&psnedf_domain_proc_info, sizeof(psnedf_domain_proc_info), 0);
	init_domain_proc_info(&psnedf_domain_proc_info, num_rt_cpus, num_rt_cpus);
	psnedf_domain_proc_info.num_cpus = num_rt_cpus;
	psnedf_domain_proc_info.num_domains = num_rt_cpus;

	for (cpu = 0, i = 0; cpu < num_online_cpus(); ++cpu) {
		if (cpu == release_master)
			continue;
		cpu_map = &psnedf_domain_proc_info.cpu_to_domains[i];
		domain_map = &psnedf_domain_proc_info.domain_to_cpus[i];

		cpu_map->id = cpu;
		domain_map->id = i; /* enumerate w/o counting the release master */
		cpumask_set_cpu(i, cpu_map->mask);
		cpumask_set_cpu(cpu, domain_map->mask);
		++i;
	}
}

static long psnedf_activate_plugin(void)
{
#ifdef CONFIG_RELEASE_MASTER
	int cpu;

	for_each_online_cpu(cpu) {
		remote_edf(cpu)->release_master = atomic_read(&release_master_cpu);
	}
#endif

#ifdef CONFIG_LITMUS_LOCKING
	get_srp_prio = psnedf_get_srp_prio;
#endif

	psnedf_setup_domain_proc();

	return 0;
}

static long psnedf_deactivate_plugin(void)
{
	destroy_domain_proc_info(&psnedf_domain_proc_info);
	return 0;
}

static long psnedf_admit_task(struct task_struct* tsk)
{
	if (task_cpu(tsk) == tsk->rt_param.task_params.cpu
#ifdef CONFIG_RELEASE_MASTER
	    /* don't allow tasks on release master CPU */
	     && task_cpu(tsk) != remote_edf(task_cpu(tsk))->release_master
#endif
		)
		return 0;
	else
		return -EINVAL;
}

/*	Plugin object	*/
static struct sched_plugin psn_edf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "PSN-EDF",
	.task_new		= psnedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= psnedf_task_exit,
	.schedule		= psnedf_schedule,
	.task_wake_up		= psnedf_task_wake_up,
	.task_block		= psnedf_task_block,
	.admit_task		= psnedf_admit_task,
	.activate_plugin	= psnedf_activate_plugin,
	.deactivate_plugin	= psnedf_deactivate_plugin,
	.get_domain_proc_info	= psnedf_get_domain_proc_info,
#ifdef CONFIG_LITMUS_LOCKING
	.allocate_lock		= psnedf_allocate_lock,
#endif
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
