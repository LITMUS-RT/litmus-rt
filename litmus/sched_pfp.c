/*
 * litmus/sched_pfp.c
 *
 * Implementation of partitioned fixed-priority scheduling.
 * Based on PSN-EDF.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/wait.h>
#include <litmus/jobs.h>
#include <litmus/preempt.h>
#include <litmus/fp_common.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>
#include <litmus/trace.h>
#include <litmus/budget.h>

#include <linux/uaccess.h>


typedef struct {
	rt_domain_t 		domain;
	struct fp_prio_queue	ready_queue;
	int          		cpu;
	struct task_struct* 	scheduled; /* only RT tasks */
/*
 * scheduling lock slock
 * protects the domain and serializes scheduling decisions
 */
#define slock domain.ready_lock

} pfp_domain_t;

DEFINE_PER_CPU(pfp_domain_t, pfp_domains);

pfp_domain_t* pfp_doms[NR_CPUS];

#define local_pfp		(&__get_cpu_var(pfp_domains))
#define remote_dom(cpu)		(&per_cpu(pfp_domains, cpu).domain)
#define remote_pfp(cpu)	(&per_cpu(pfp_domains, cpu))
#define task_dom(task)		remote_dom(get_partition(task))
#define task_pfp(task)		remote_pfp(get_partition(task))

/* we assume the lock is being held */
static void preempt(pfp_domain_t *pfp)
{
	preempt_if_preemptable(pfp->scheduled, pfp->cpu);
}

static unsigned int priority_index(struct task_struct* t)
{
#ifdef CONFIG_LITMUS_LOCKING
	if (unlikely(t->rt_param.inh_task))
		/* use effective priority */
		t = t->rt_param.inh_task;

	if (is_priority_boosted(t)) {
		/* zero is reserved for priority-boosted tasks */
		return 0;
	} else
#endif
		return get_priority(t);
}


static void pfp_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	pfp_domain_t *pfp = container_of(rt, pfp_domain_t, domain);
	unsigned long flags;
	struct task_struct* t;
	struct bheap_node* hn;

	raw_spin_lock_irqsave(&pfp->slock, flags);

	while (!bheap_empty(tasks)) {
		hn = bheap_take(fp_ready_order, tasks);
		t = bheap2task(hn);
		TRACE_TASK(t, "released (part:%d prio:%d)\n",
			   get_partition(t), get_priority(t));
		fp_prio_add(&pfp->ready_queue, t, priority_index(t));
	}

	/* do we need to preempt? */
	if (fp_higher_prio(fp_prio_peek(&pfp->ready_queue), pfp->scheduled)) {
		TRACE_CUR("preempted by new release\n");
		preempt(pfp);
	}

	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

static void pfp_preempt_check(pfp_domain_t *pfp)
{
	if (fp_higher_prio(fp_prio_peek(&pfp->ready_queue), pfp->scheduled))
		preempt(pfp);
}

static void pfp_domain_init(pfp_domain_t* pfp,
			       int cpu)
{
	fp_domain_init(&pfp->domain, NULL, pfp_release_jobs);
	pfp->cpu      		= cpu;
	pfp->scheduled		= NULL;
	fp_prio_queue_init(&pfp->ready_queue);
}

static void requeue(struct task_struct* t, pfp_domain_t *pfp)
{
	BUG_ON(!is_running(t));

	tsk_rt(t)->completed = 0;
	if (is_released(t, litmus_clock()))
		fp_prio_add(&pfp->ready_queue, t, priority_index(t));
	else
		add_release(&pfp->domain, t); /* it has got to wait */
}

static void job_completion(struct task_struct* t, int forced)
{
	sched_trace_task_completion(t,forced);
	TRACE_TASK(t, "job_completion().\n");

	tsk_rt(t)->completed = 1;
	prepare_for_next_period(t);
	if (is_released(t, litmus_clock()))
		sched_trace_task_release(t);
}

static void pfp_tick(struct task_struct *t)
{
	pfp_domain_t *pfp = local_pfp;

	/* Check for inconsistency. We don't need the lock for this since
	 * ->scheduled is only changed in schedule, which obviously is not
	 *  executing in parallel on this CPU
	 */
	BUG_ON(is_realtime(t) && t != pfp->scheduled);

	if (is_realtime(t) && budget_enforced(t) && budget_exhausted(t)) {
		if (!is_np(t)) {
			litmus_reschedule_local();
			TRACE("pfp_scheduler_tick: "
			      "%d is preemptable "
			      " => FORCE_RESCHED\n", t->pid);
		} else if (is_user_np(t)) {
			TRACE("pfp_scheduler_tick: "
			      "%d is non-preemptable, "
			      "preemption delayed.\n", t->pid);
			request_exit_np(t);
		}
	}
}

static struct task_struct* pfp_schedule(struct task_struct * prev)
{
	pfp_domain_t* 	pfp = local_pfp;
	struct task_struct*	next;

	int out_of_time, sleep, preempt, np, exists, blocks, resched, migrate;

	raw_spin_lock(&pfp->slock);

	/* sanity checking
	 * differently from gedf, when a task exits (dead)
	 * pfp->schedule may be null and prev _is_ realtime
	 */
	BUG_ON(pfp->scheduled && pfp->scheduled != prev);
	BUG_ON(pfp->scheduled && !is_realtime(prev));

	/* (0) Determine state */
	exists      = pfp->scheduled != NULL;
	blocks      = exists && !is_running(pfp->scheduled);
	out_of_time = exists &&
				  budget_enforced(pfp->scheduled) &&
				  budget_exhausted(pfp->scheduled);
	np 	    = exists && is_np(pfp->scheduled);
	sleep	    = exists && is_completed(pfp->scheduled);
	migrate     = exists && get_partition(pfp->scheduled) != pfp->cpu;
	preempt     = !blocks && (migrate || fp_preemption_needed(&pfp->ready_queue, prev));

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
		request_exit_np(pfp->scheduled);

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this.
	 */
	if (!np && (out_of_time || sleep) && !blocks && !migrate) {
		job_completion(pfp->scheduled, !sleep);
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
		if (pfp->scheduled && !blocks  && !migrate)
			requeue(pfp->scheduled, pfp);
		next = fp_prio_take(&pfp->ready_queue);
		if (next == prev) {
			struct task_struct *t = fp_prio_peek(&pfp->ready_queue);
			TRACE_TASK(next, "next==prev sleep=%d oot=%d np=%d preempt=%d migrate=%d "
				   "boost=%d empty=%d prio-idx=%u prio=%u\n",
				   sleep, out_of_time, np, preempt, migrate,
				   is_priority_boosted(next),
				   t == NULL,
				   priority_index(next),
				   get_priority(next));
			if (t)
				TRACE_TASK(t, "waiter boost=%d prio-idx=%u prio=%u\n",
					   is_priority_boosted(t),
					   priority_index(t),
					   get_priority(t));
		}
		/* If preempt is set, we should not see the same task again. */
		BUG_ON(preempt && next == prev);
		/* Similarly, if preempt is set, then next may not be NULL,
		 * unless it's a migration. */
		BUG_ON(preempt && !migrate && next == NULL);
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	if (next) {
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
		tsk_rt(next)->completed = 0;
	} else {
		TRACE("becoming idle at %llu\n", litmus_clock());
	}

	pfp->scheduled = next;
	sched_state_task_picked();
	raw_spin_unlock(&pfp->slock);

	return next;
}

#ifdef CONFIG_LITMUS_LOCKING

/* prev is no longer scheduled --- see if it needs to migrate */
static void pfp_finish_switch(struct task_struct *prev)
{
	pfp_domain_t *to;

	if (is_realtime(prev) &&
	    is_running(prev) &&
	    get_partition(prev) != smp_processor_id()) {
		TRACE_TASK(prev, "needs to migrate from P%d to P%d\n",
			   smp_processor_id(), get_partition(prev));

		to = task_pfp(prev);

		raw_spin_lock(&to->slock);

		TRACE_TASK(prev, "adding to queue on P%d\n", to->cpu);
		requeue(prev, to);
		if (fp_preemption_needed(&to->ready_queue, to->scheduled))
			preempt(to);

		raw_spin_unlock(&to->slock);

	}
}

#endif

/*	Prepare a task for running in RT mode
 */
static void pfp_task_new(struct task_struct * t, int on_rq, int running)
{
	pfp_domain_t* 	pfp = task_pfp(t);
	unsigned long		flags;

	TRACE_TASK(t, "P-FP: task new, cpu = %d\n",
		   t->rt_param.task_params.cpu);

	/* setup job parameters */
	release_at(t, litmus_clock());

	/* The task should be running in the queue, otherwise signal
	 * code will try to wake it up with fatal consequences.
	 */
	raw_spin_lock_irqsave(&pfp->slock, flags);
	if (running) {
		/* there shouldn't be anything else running at the time */
		BUG_ON(pfp->scheduled);
		pfp->scheduled = t;
	} else {
		requeue(t, pfp);
		/* maybe we have to reschedule */
		pfp_preempt_check(pfp);
	}
	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

static void pfp_task_wake_up(struct task_struct *task)
{
	unsigned long		flags;
	pfp_domain_t*		pfp = task_pfp(task);
	lt_t			now;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());
	raw_spin_lock_irqsave(&pfp->slock, flags);

#ifdef CONFIG_LITMUS_LOCKING
	/* Should only be queued when processing a fake-wake up due to a
	 * migration-related state change. */
	if (unlikely(is_queued(task))) {
		TRACE_TASK(task, "WARNING: waking task still queued. Is this right?\n");
		goto out_unlock;
	}
#else
	BUG_ON(is_queued(task));
#endif
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
	 * and won. Also, don't requeue if it is still queued, which can
	 * happen under the DPCP due wake-ups racing with migrations.
	 */
	if (pfp->scheduled != task) {
		requeue(task, pfp);
		pfp_preempt_check(pfp);
	}

#ifdef CONFIG_LITMUS_LOCKING
out_unlock:
#endif
	raw_spin_unlock_irqrestore(&pfp->slock, flags);
	TRACE_TASK(task, "wake up done\n");
}

static void pfp_task_block(struct task_struct *t)
{
	/* only running tasks can block, thus t is in no queue */
	TRACE_TASK(t, "block at %llu, state=%d\n", litmus_clock(), t->state);

	BUG_ON(!is_realtime(t));

	/* If this task blocked normally, it shouldn't be queued. The exception is
	 * if this is a simulated block()/wakeup() pair from the pull-migration code path.
	 * This should only happen if the DPCP is being used.
	 */
#ifdef CONFIG_LITMUS_LOCKING
	if (unlikely(is_queued(t)))
		TRACE_TASK(t, "WARNING: blocking task still queued. Is this right?\n");
#else
	BUG_ON(is_queued(t));
#endif
}

static void pfp_task_exit(struct task_struct * t)
{
	unsigned long flags;
	pfp_domain_t* 	pfp = task_pfp(t);
	rt_domain_t*		dom;

	raw_spin_lock_irqsave(&pfp->slock, flags);
	if (is_queued(t)) {
		BUG(); /* This currently doesn't work. */
		/* dequeue */
		dom  = task_dom(t);
		remove(dom, t);
	}
	if (pfp->scheduled == t) {
		pfp->scheduled = NULL;
		preempt(pfp);
	}
	TRACE_TASK(t, "RIP, now reschedule\n");

	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

#ifdef CONFIG_LITMUS_LOCKING

#include <litmus/fdso.h>
#include <litmus/srp.h>

static void fp_dequeue(pfp_domain_t* pfp, struct task_struct* t)
{
	BUG_ON(pfp->scheduled == t && is_queued(t));
	if (is_queued(t))
		fp_prio_remove(&pfp->ready_queue, t, priority_index(t));
}

static void fp_set_prio_inh(pfp_domain_t* pfp, struct task_struct* t,
			    struct task_struct* prio_inh)
{
	int requeue;

	if (!t || t->rt_param.inh_task == prio_inh) {
		/* no update  required */
		if (t)
			TRACE_TASK(t, "no prio-inh update required\n");
		return;
	}

	requeue = is_queued(t);
	TRACE_TASK(t, "prio-inh: is_queued:%d\n", requeue);

	if (requeue)
		/* first remove */
		fp_dequeue(pfp, t);

	t->rt_param.inh_task = prio_inh;

	if (requeue)
		/* add again to the right queue */
		fp_prio_add(&pfp->ready_queue, t, priority_index(t));
}

static int effective_agent_priority(int prio)
{
	/* make sure agents have higher priority */
	return prio - LITMUS_MAX_PRIORITY;
}

static lt_t prio_point(int eprio)
{
	/* make sure we have non-negative prio points */
	return eprio + LITMUS_MAX_PRIORITY;
}

static int prio_from_point(lt_t prio_point)
{
	return ((int) prio_point) - LITMUS_MAX_PRIORITY;
}

static void boost_priority(struct task_struct* t, lt_t priority_point)
{
	unsigned long		flags;
	pfp_domain_t* 	pfp = task_pfp(t);

	raw_spin_lock_irqsave(&pfp->slock, flags);


	TRACE_TASK(t, "priority boosted at %llu\n", litmus_clock());

	tsk_rt(t)->priority_boosted = 1;
	/* tie-break by protocol-specific priority point */
	tsk_rt(t)->boost_start_time = priority_point;

	/* Priority boosting currently only takes effect for already-scheduled
	 * tasks. This is sufficient since priority boosting only kicks in as
	 * part of lock acquisitions. */
	BUG_ON(pfp->scheduled != t);

	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

static void unboost_priority(struct task_struct* t)
{
	unsigned long		flags;
	pfp_domain_t* 	pfp = task_pfp(t);
	lt_t			now;

	raw_spin_lock_irqsave(&pfp->slock, flags);
	now = litmus_clock();

	/* assumption: this only happens when the job is scheduled */
	BUG_ON(pfp->scheduled != t);

	TRACE_TASK(t, "priority restored at %llu\n", now);

	/* priority boosted jobs must be scheduled */
	BUG_ON(pfp->scheduled != t);

	tsk_rt(t)->priority_boosted = 0;
	tsk_rt(t)->boost_start_time = 0;

	/* check if this changes anything */
	if (fp_preemption_needed(&pfp->ready_queue, pfp->scheduled))
		preempt(pfp);

	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

/* ******************** SRP support ************************ */

static unsigned int pfp_get_srp_prio(struct task_struct* t)
{
	return get_priority(t);
}

/* ******************** FMLP support ********************** */

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
int pfp_fmlp_lock(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct fmlp_semaphore *sem = fmlp_from_lock(l);
	wait_queue_t wait;
	unsigned long flags;
	lt_t time_of_request;

	if (!is_realtime(t))
		return -EPERM;

	/* prevent nested lock acquisition --- not supported by FMLP */
	if (tsk_rt(t)->num_locks_held ||
	    tsk_rt(t)->num_local_locks_held)
		return -EBUSY;

	spin_lock_irqsave(&sem->wait.lock, flags);

	/* tie-break by this point in time */
	time_of_request = litmus_clock();

	/* Priority-boost ourself *before* we suspend so that
	 * our priority is boosted when we resume. */
	boost_priority(t, time_of_request);

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

		spin_unlock_irqrestore(&sem->wait.lock, flags);
	}

	tsk_rt(t)->num_locks_held++;

	return 0;
}

int pfp_fmlp_unlock(struct litmus_lock* l)
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
		/* next becomes the resouce holder */
		sem->owner = next;

		/* Wake up next. The waiting job is already priority-boosted. */
		wake_up_process(next);
	} else
		/* resource becomes available */
		sem->owner = NULL;

out:
	spin_unlock_irqrestore(&sem->wait.lock, flags);
	return err;
}

int pfp_fmlp_close(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct fmlp_semaphore *sem = fmlp_from_lock(l);
	unsigned long flags;

	int owner;

	spin_lock_irqsave(&sem->wait.lock, flags);

	owner = sem->owner == t;

	spin_unlock_irqrestore(&sem->wait.lock, flags);

	if (owner)
		pfp_fmlp_unlock(l);

	return 0;
}

void pfp_fmlp_free(struct litmus_lock* lock)
{
	kfree(fmlp_from_lock(lock));
}

static struct litmus_lock_ops pfp_fmlp_lock_ops = {
	.close  = pfp_fmlp_close,
	.lock   = pfp_fmlp_lock,
	.unlock = pfp_fmlp_unlock,
	.deallocate = pfp_fmlp_free,
};

static struct litmus_lock* pfp_new_fmlp(void)
{
	struct fmlp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	sem->owner   = NULL;
	init_waitqueue_head(&sem->wait);
	sem->litmus_lock.ops = &pfp_fmlp_lock_ops;

	return &sem->litmus_lock;
}

/* ******************** MPCP support ********************** */

struct mpcp_semaphore {
	struct litmus_lock litmus_lock;

	/* current resource holder */
	struct task_struct *owner;

	/* priority queue of waiting tasks */
	wait_queue_head_t wait;

	/* priority ceiling per cpu */
	unsigned int prio_ceiling[NR_CPUS];

	/* should jobs spin "virtually" for this resource? */
	int vspin;
};

#define OMEGA_CEILING UINT_MAX

/* Since jobs spin "virtually" while waiting to acquire a lock,
 * they first must aquire a local per-cpu resource.
 */
static DEFINE_PER_CPU(wait_queue_head_t, mpcpvs_vspin_wait);
static DEFINE_PER_CPU(struct task_struct*, mpcpvs_vspin);

/* called with preemptions off <=> no local modifications */
static void mpcp_vspin_enter(void)
{
	struct task_struct* t = current;

	while (1) {
		if (__get_cpu_var(mpcpvs_vspin) == NULL) {
			/* good, we get to issue our request */
			__get_cpu_var(mpcpvs_vspin) = t;
			break;
		} else {
			/* some job is spinning => enqueue in request queue */
			prio_wait_queue_t wait;
			wait_queue_head_t* vspin = &__get_cpu_var(mpcpvs_vspin_wait);
			unsigned long flags;

			/* ordered by regular priority */
			init_prio_waitqueue_entry(&wait, t, prio_point(get_priority(t)));

			spin_lock_irqsave(&vspin->lock, flags);

			set_task_state(t, TASK_UNINTERRUPTIBLE);

			__add_wait_queue_prio_exclusive(vspin, &wait);

			spin_unlock_irqrestore(&vspin->lock, flags);

			TS_LOCK_SUSPEND;

			preempt_enable_no_resched();

			schedule();

			preempt_disable();

			TS_LOCK_RESUME;
			/* Recheck if we got it --- some higher-priority process might
			 * have swooped in. */
		}
	}
	/* ok, now it is ours */
}

/* called with preemptions off */
static void mpcp_vspin_exit(void)
{
	struct task_struct* t = current, *next;
	unsigned long flags;
	wait_queue_head_t* vspin = &__get_cpu_var(mpcpvs_vspin_wait);

	BUG_ON(__get_cpu_var(mpcpvs_vspin) != t);

	/* no spinning job */
	__get_cpu_var(mpcpvs_vspin) = NULL;

	/* see if anyone is waiting for us to stop "spinning" */
	spin_lock_irqsave(&vspin->lock, flags);
	next = __waitqueue_remove_first(vspin);

	if (next)
		wake_up_process(next);

	spin_unlock_irqrestore(&vspin->lock, flags);
}

static inline struct mpcp_semaphore* mpcp_from_lock(struct litmus_lock* lock)
{
	return container_of(lock, struct mpcp_semaphore, litmus_lock);
}

int pfp_mpcp_lock(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct mpcp_semaphore *sem = mpcp_from_lock(l);
	prio_wait_queue_t wait;
	unsigned long flags;

	if (!is_realtime(t))
		return -EPERM;

	/* prevent nested lock acquisition */
	if (tsk_rt(t)->num_locks_held ||
	    tsk_rt(t)->num_local_locks_held)
		return -EBUSY;

	preempt_disable();

	if (sem->vspin)
		mpcp_vspin_enter();

	/* Priority-boost ourself *before* we suspend so that
	 * our priority is boosted when we resume. Use the priority
	 * ceiling for the local partition. */
	boost_priority(t, sem->prio_ceiling[get_partition(t)]);

	spin_lock_irqsave(&sem->wait.lock, flags);

	preempt_enable_no_resched();

	if (sem->owner) {
		/* resource is not free => must suspend and wait */

		/* ordered by regular priority */
		init_prio_waitqueue_entry(&wait, t, prio_point(get_priority(t)));

		/* FIXME: interruptible would be nice some day */
		set_task_state(t, TASK_UNINTERRUPTIBLE);

		__add_wait_queue_prio_exclusive(&sem->wait, &wait);

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

		spin_unlock_irqrestore(&sem->wait.lock, flags);
	}

	tsk_rt(t)->num_locks_held++;

	return 0;
}

int pfp_mpcp_unlock(struct litmus_lock* l)
{
	struct task_struct *t = current, *next;
	struct mpcp_semaphore *sem = mpcp_from_lock(l);
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
		/* next becomes the resouce holder */
		sem->owner = next;

		/* Wake up next. The waiting job is already priority-boosted. */
		wake_up_process(next);
	} else
		/* resource becomes available */
		sem->owner = NULL;

out:
	spin_unlock_irqrestore(&sem->wait.lock, flags);

	if (sem->vspin && err == 0) {
		preempt_disable();
		mpcp_vspin_exit();
		preempt_enable();
	}

	return err;
}

int pfp_mpcp_open(struct litmus_lock* l, void* config)
{
	struct task_struct *t = current;
	struct mpcp_semaphore *sem = mpcp_from_lock(l);
	int cpu, local_cpu;
	unsigned long flags;

	if (!is_realtime(t))
		/* we need to know the real-time priority */
		return -EPERM;

	local_cpu = get_partition(t);

	spin_lock_irqsave(&sem->wait.lock, flags);

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		if (cpu != local_cpu)
		{
			sem->prio_ceiling[cpu] = min(sem->prio_ceiling[cpu],
						     get_priority(t));
			TRACE_CUR("priority ceiling for sem %p is now %d on cpu %d\n",
				  sem, sem->prio_ceiling[cpu], cpu);
		}

	spin_unlock_irqrestore(&sem->wait.lock, flags);

	return 0;
}

int pfp_mpcp_close(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct mpcp_semaphore *sem = mpcp_from_lock(l);
	unsigned long flags;

	int owner;

	spin_lock_irqsave(&sem->wait.lock, flags);

	owner = sem->owner == t;

	spin_unlock_irqrestore(&sem->wait.lock, flags);

	if (owner)
		pfp_mpcp_unlock(l);

	return 0;
}

void pfp_mpcp_free(struct litmus_lock* lock)
{
	kfree(mpcp_from_lock(lock));
}

static struct litmus_lock_ops pfp_mpcp_lock_ops = {
	.close  = pfp_mpcp_close,
	.lock   = pfp_mpcp_lock,
	.open	= pfp_mpcp_open,
	.unlock = pfp_mpcp_unlock,
	.deallocate = pfp_mpcp_free,
};

static struct litmus_lock* pfp_new_mpcp(int vspin)
{
	struct mpcp_semaphore* sem;
	int cpu;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	sem->owner   = NULL;
	init_waitqueue_head(&sem->wait);
	sem->litmus_lock.ops = &pfp_mpcp_lock_ops;

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		sem->prio_ceiling[cpu] = OMEGA_CEILING;

	/* mark as virtual spinning */
	sem->vspin = vspin;

	return &sem->litmus_lock;
}


/* ******************** PCP support ********************** */


struct pcp_semaphore {
	struct litmus_lock litmus_lock;

	struct list_head ceiling;

	/* current resource holder */
	struct task_struct *owner;

	/* priority ceiling --- can be negative due to DPCP support */
	int prio_ceiling;

	/* on which processor is this PCP semaphore allocated? */
	int on_cpu;
};

static inline struct pcp_semaphore* pcp_from_lock(struct litmus_lock* lock)
{
	return container_of(lock, struct pcp_semaphore, litmus_lock);
}


struct pcp_state {
	struct list_head system_ceiling;

	/* highest-priority waiting task */
	struct task_struct* hp_waiter;

	/* list of jobs waiting to get past the system ceiling */
	wait_queue_head_t ceiling_blocked;
};

static void pcp_init_state(struct pcp_state* s)
{
	INIT_LIST_HEAD(&s->system_ceiling);
	s->hp_waiter = NULL;
	init_waitqueue_head(&s->ceiling_blocked);
}

static DEFINE_PER_CPU(struct pcp_state, pcp_state);

/* assumes preemptions are off */
static struct pcp_semaphore* pcp_get_ceiling(void)
{
	struct list_head* top = __get_cpu_var(pcp_state).system_ceiling.next;

	if (top)
		return list_entry(top, struct pcp_semaphore, ceiling);
	else
		return NULL;
}

/* assumes preempt off */
static void pcp_add_ceiling(struct pcp_semaphore* sem)
{
	struct list_head *pos;
	struct list_head *in_use = &__get_cpu_var(pcp_state).system_ceiling;
	struct pcp_semaphore* held;

	BUG_ON(sem->on_cpu != smp_processor_id());
	BUG_ON(in_list(&sem->ceiling));

	list_for_each(pos, in_use) {
		held = list_entry(pos, struct pcp_semaphore, ceiling);
		if (held->prio_ceiling >= sem->prio_ceiling) {
			__list_add(&sem->ceiling, pos->prev, pos);
			return;
		}
	}

	/* we hit the end of the list */

	list_add_tail(&sem->ceiling, in_use);
}

/* assumes preempt off */
static int pcp_exceeds_ceiling(struct pcp_semaphore* ceiling,
			      struct task_struct* task,
			      int effective_prio)
{
	return ceiling == NULL ||
		ceiling->prio_ceiling > effective_prio ||
		ceiling->owner == task;
}

/* assumes preempt off */
static void pcp_priority_inheritance(void)
{
	unsigned long	flags;
	pfp_domain_t* 	pfp = local_pfp;

	struct pcp_semaphore* ceiling = pcp_get_ceiling();
	struct task_struct *blocker, *blocked;

	blocker = ceiling ?  ceiling->owner : NULL;
	blocked = __get_cpu_var(pcp_state).hp_waiter;

	raw_spin_lock_irqsave(&pfp->slock, flags);

	/* Current is no longer inheriting anything by default.  This should be
	 * the currently scheduled job, and hence not currently queued. */
	BUG_ON(current != pfp->scheduled);

	fp_set_prio_inh(pfp, current, NULL);
	fp_set_prio_inh(pfp, blocked, NULL);
	fp_set_prio_inh(pfp, blocker, NULL);


	/* Let blocking job inherit priority of blocked job, if required. */
	if (blocker && blocked &&
	    fp_higher_prio(blocked, blocker)) {
		TRACE_TASK(blocker, "PCP inherits from %s/%d (prio %u -> %u) \n",
			   blocked->comm, blocked->pid,
			   get_priority(blocker), get_priority(blocked));
		fp_set_prio_inh(pfp, blocker, blocked);
	}

	/* Check if anything changed. If the blocked job is current, then it is
	 * just blocking and hence is going to call the scheduler anyway. */
	if (blocked != current &&
	    fp_higher_prio(fp_prio_peek(&pfp->ready_queue), pfp->scheduled))
		preempt(pfp);

	raw_spin_unlock_irqrestore(&pfp->slock, flags);
}

/* called with preemptions off */
static void pcp_raise_ceiling(struct pcp_semaphore* sem,
			      int effective_prio)
{
	struct task_struct* t = current;
	struct pcp_semaphore* ceiling;
	prio_wait_queue_t wait;
	unsigned int waiting_higher_prio;

	do {
		ceiling = pcp_get_ceiling();
		if (pcp_exceeds_ceiling(ceiling, t, effective_prio))
			break;

		TRACE_CUR("PCP ceiling-blocked, wanted sem %p, but %s/%d has the ceiling \n",
			  sem, ceiling->owner->comm, ceiling->owner->pid);

		/* we need to wait until the ceiling is lowered */

		/* enqueue in priority order */
		init_prio_waitqueue_entry(&wait, t, prio_point(effective_prio));
		set_task_state(t, TASK_UNINTERRUPTIBLE);
		waiting_higher_prio = add_wait_queue_prio_exclusive(
			&__get_cpu_var(pcp_state).ceiling_blocked, &wait);

		if (waiting_higher_prio == 0) {
			TRACE_CUR("PCP new highest-prio waiter => prio inheritance\n");

			/* we are the new highest-priority waiting job
			 * => update inheritance */
			__get_cpu_var(pcp_state).hp_waiter = t;
			pcp_priority_inheritance();
		}

		TS_LOCK_SUSPEND;

		preempt_enable_no_resched();
		schedule();
		preempt_disable();

		/* pcp_resume_unblocked() removed us from wait queue */

		TS_LOCK_RESUME;
	} while(1);

	TRACE_CUR("PCP got the ceiling and sem %p\n", sem);

	/* We are good to go. The semaphore should be available. */
	BUG_ON(sem->owner != NULL);

	sem->owner = t;

	pcp_add_ceiling(sem);
}

static void pcp_resume_unblocked(void)
{
	wait_queue_head_t *blocked =  &__get_cpu_var(pcp_state).ceiling_blocked;
	unsigned long flags;
	prio_wait_queue_t* q;
	struct task_struct* t = NULL;

	struct pcp_semaphore* ceiling = pcp_get_ceiling();

	spin_lock_irqsave(&blocked->lock, flags);

	while (waitqueue_active(blocked)) {
		/* check first == highest-priority waiting job */
		q = list_entry(blocked->task_list.next,
			       prio_wait_queue_t, wq.task_list);
		t = (struct task_struct*) q->wq.private;

		/* can it proceed now? => let it go */
		if (pcp_exceeds_ceiling(ceiling, t,
					prio_from_point(q->priority))) {
		    __remove_wait_queue(blocked, &q->wq);
		    wake_up_process(t);
		} else {
			/* We are done. Update highest-priority waiter. */
			__get_cpu_var(pcp_state).hp_waiter = t;
			goto out;
		}
	}
	/* If we get here, then there are no more waiting
	 * jobs. */
	__get_cpu_var(pcp_state).hp_waiter = NULL;
out:
	spin_unlock_irqrestore(&blocked->lock, flags);
}

/* assumes preempt off */
static void pcp_lower_ceiling(struct pcp_semaphore* sem)
{
	BUG_ON(!in_list(&sem->ceiling));
	BUG_ON(sem->owner != current);
	BUG_ON(sem->on_cpu != smp_processor_id());

	/* remove from ceiling list */
	list_del(&sem->ceiling);

	/* release */
	sem->owner = NULL;

	TRACE_CUR("PCP released sem %p\n", sem);

	pcp_priority_inheritance();

	/* Wake up all ceiling-blocked jobs that now pass the ceiling. */
	pcp_resume_unblocked();
}

static void pcp_update_prio_ceiling(struct pcp_semaphore* sem,
				    int effective_prio)
{
	/* This needs to be synchronized on something.
	 * Might as well use waitqueue lock for the processor.
	 * We assume this happens only before the task set starts execution,
	 * (i.e., during initialization), but it may happen on multiple processors
	 * at the same time.
	 */
	unsigned long flags;

	struct pcp_state* s = &per_cpu(pcp_state, sem->on_cpu);

	spin_lock_irqsave(&s->ceiling_blocked.lock, flags);

	sem->prio_ceiling = min(sem->prio_ceiling, effective_prio);

	spin_unlock_irqrestore(&s->ceiling_blocked.lock, flags);
}

static void pcp_init_semaphore(struct pcp_semaphore* sem, int cpu)
{
	sem->owner   = NULL;
	INIT_LIST_HEAD(&sem->ceiling);
	sem->prio_ceiling = INT_MAX;
	sem->on_cpu = cpu;
}

int pfp_pcp_lock(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct pcp_semaphore *sem = pcp_from_lock(l);

	int eprio = effective_agent_priority(get_priority(t));
	int from  = get_partition(t);
	int to    = sem->on_cpu;

	if (!is_realtime(t) || from != to)
		return -EPERM;

	/* prevent nested lock acquisition in global critical section */
	if (tsk_rt(t)->num_locks_held)
		return -EBUSY;

	preempt_disable();

	pcp_raise_ceiling(sem, eprio);

	preempt_enable();

	tsk_rt(t)->num_local_locks_held++;

	return 0;
}

int pfp_pcp_unlock(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct pcp_semaphore *sem = pcp_from_lock(l);

	int err = 0;

	preempt_disable();

	if (sem->on_cpu != smp_processor_id() || sem->owner != t) {
		err = -EINVAL;
		goto out;
	}

	tsk_rt(t)->num_local_locks_held--;

	/* give it back */
	pcp_lower_ceiling(sem);

out:
	preempt_enable();

	return err;
}

int pfp_pcp_open(struct litmus_lock* l, void* __user config)
{
	struct task_struct *t = current;
	struct pcp_semaphore *sem = pcp_from_lock(l);

	int cpu, eprio;

	if (!is_realtime(t))
		/* we need to know the real-time priority */
		return -EPERM;

	if (!config)
		cpu = get_partition(t);
	else if (get_user(cpu, (int*) config))
		return -EFAULT;

	/* make sure the resource location matches */
	if (cpu != sem->on_cpu)
		return -EINVAL;

	eprio = effective_agent_priority(get_priority(t));

	pcp_update_prio_ceiling(sem, eprio);

	return 0;
}

int pfp_pcp_close(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct pcp_semaphore *sem = pcp_from_lock(l);

	int owner = 0;

	preempt_disable();

	if (sem->on_cpu == smp_processor_id())
		owner = sem->owner == t;

	preempt_enable();

	if (owner)
		pfp_pcp_unlock(l);

	return 0;
}

void pfp_pcp_free(struct litmus_lock* lock)
{
	kfree(pcp_from_lock(lock));
}


static struct litmus_lock_ops pfp_pcp_lock_ops = {
	.close  = pfp_pcp_close,
	.lock   = pfp_pcp_lock,
	.open	= pfp_pcp_open,
	.unlock = pfp_pcp_unlock,
	.deallocate = pfp_pcp_free,
};


static struct litmus_lock* pfp_new_pcp(int on_cpu)
{
	struct pcp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	sem->litmus_lock.ops = &pfp_pcp_lock_ops;
	pcp_init_semaphore(sem, on_cpu);

	return &sem->litmus_lock;
}

/* ******************** DPCP support ********************** */

struct dpcp_semaphore {
	struct litmus_lock litmus_lock;
	struct pcp_semaphore  pcp;
	int owner_cpu;
};

static inline struct dpcp_semaphore* dpcp_from_lock(struct litmus_lock* lock)
{
	return container_of(lock, struct dpcp_semaphore, litmus_lock);
}

/* called with preemptions disabled */
static void pfp_migrate_to(int target_cpu)
{
	struct task_struct* t = current;
	pfp_domain_t *from;

	if (get_partition(t) == target_cpu)
		return;

	/* make sure target_cpu makes sense */
	BUG_ON(!cpu_online(target_cpu));

	local_irq_disable();

	/* scheduled task should not be in any ready or release queue */
	BUG_ON(is_queued(t));

	/* lock both pfp domains in order of address */
	from = task_pfp(t);

	raw_spin_lock(&from->slock);

	/* switch partitions */
	tsk_rt(t)->task_params.cpu = target_cpu;

	raw_spin_unlock(&from->slock);

	/* Don't trace scheduler costs as part of
	 * locking overhead. Scheduling costs are accounted for
	 * explicitly. */
	TS_LOCK_SUSPEND;

	local_irq_enable();
	preempt_enable_no_resched();

	/* deschedule to be migrated */
	schedule();

	/* we are now on the target processor */
	preempt_disable();

	/* start recording costs again */
	TS_LOCK_RESUME;

	BUG_ON(smp_processor_id() != target_cpu);
}

int pfp_dpcp_lock(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct dpcp_semaphore *sem = dpcp_from_lock(l);
	int eprio = effective_agent_priority(get_priority(t));
	int from  = get_partition(t);
	int to    = sem->pcp.on_cpu;

	if (!is_realtime(t))
		return -EPERM;

	/* prevent nested lock accquisition */
	if (tsk_rt(t)->num_locks_held ||
	    tsk_rt(t)->num_local_locks_held)
		return -EBUSY;

	preempt_disable();

	/* Priority-boost ourself *before* we suspend so that
	 * our priority is boosted when we resume. */

	boost_priority(t, get_priority(t));

	pfp_migrate_to(to);

	pcp_raise_ceiling(&sem->pcp, eprio);

	/* yep, we got it => execute request */
	sem->owner_cpu = from;

	preempt_enable();

	tsk_rt(t)->num_locks_held++;

	return 0;
}

int pfp_dpcp_unlock(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct dpcp_semaphore *sem = dpcp_from_lock(l);
	int err = 0;
	int home;

	preempt_disable();

	if (sem->pcp.on_cpu != smp_processor_id() || sem->pcp.owner != t) {
		err = -EINVAL;
		goto out;
	}

	tsk_rt(t)->num_locks_held--;

	home = sem->owner_cpu;

	/* give it back */
	pcp_lower_ceiling(&sem->pcp);

	/* we lose the benefit of priority boosting */
	unboost_priority(t);

	pfp_migrate_to(home);

out:
	preempt_enable();

	return err;
}

int pfp_dpcp_open(struct litmus_lock* l, void* __user config)
{
	struct task_struct *t = current;
	struct dpcp_semaphore *sem = dpcp_from_lock(l);
	int cpu, eprio;

	if (!is_realtime(t))
		/* we need to know the real-time priority */
		return -EPERM;

	if (get_user(cpu, (int*) config))
		return -EFAULT;

	/* make sure the resource location matches */
	if (cpu != sem->pcp.on_cpu)
		return -EINVAL;

	eprio = effective_agent_priority(get_priority(t));

	pcp_update_prio_ceiling(&sem->pcp, eprio);

	return 0;
}

int pfp_dpcp_close(struct litmus_lock* l)
{
	struct task_struct *t = current;
	struct dpcp_semaphore *sem = dpcp_from_lock(l);
	int owner = 0;

	preempt_disable();

	if (sem->pcp.on_cpu == smp_processor_id())
		owner = sem->pcp.owner == t;

	preempt_enable();

	if (owner)
		pfp_dpcp_unlock(l);

	return 0;
}

void pfp_dpcp_free(struct litmus_lock* lock)
{
	kfree(dpcp_from_lock(lock));
}

static struct litmus_lock_ops pfp_dpcp_lock_ops = {
	.close  = pfp_dpcp_close,
	.lock   = pfp_dpcp_lock,
	.open	= pfp_dpcp_open,
	.unlock = pfp_dpcp_unlock,
	.deallocate = pfp_dpcp_free,
};

static struct litmus_lock* pfp_new_dpcp(int on_cpu)
{
	struct dpcp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	sem->litmus_lock.ops = &pfp_dpcp_lock_ops;
	sem->owner_cpu = NO_CPU;
	pcp_init_semaphore(&sem->pcp, on_cpu);

	return &sem->litmus_lock;
}


/* **** lock constructor **** */


static long pfp_allocate_lock(struct litmus_lock **lock, int type,
				 void* __user config)
{
	int err = -ENXIO, cpu;
	struct srp_semaphore* srp;

	/* P-FP currently supports the SRP for local resources and the FMLP
	 * for global resources. */
	switch (type) {
	case FMLP_SEM:
		/* FIFO Mutex Locking Protocol */
		*lock = pfp_new_fmlp();
		if (*lock)
			err = 0;
		else
			err = -ENOMEM;
		break;

	case MPCP_SEM:
		/* Multiprocesor Priority Ceiling Protocol */
		*lock = pfp_new_mpcp(0);
		if (*lock)
			err = 0;
		else
			err = -ENOMEM;
		break;

	case MPCP_VS_SEM:
		/* Multiprocesor Priority Ceiling Protocol with virtual spinning */
		*lock = pfp_new_mpcp(1);
		if (*lock)
			err = 0;
		else
			err = -ENOMEM;
		break;

	case DPCP_SEM:
		/* Distributed Priority Ceiling Protocol */
		if (get_user(cpu, (int*) config))
			return -EFAULT;

		if (!cpu_online(cpu))
			return -EINVAL;

		*lock = pfp_new_dpcp(cpu);
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

        case PCP_SEM:
		/* Priority Ceiling Protocol */
		if (!config)
			cpu = get_partition(current);
		else if (get_user(cpu, (int*) config))
			return -EFAULT;

		if (!cpu_online(cpu))
			return -EINVAL;

		*lock = pfp_new_pcp(cpu);
		if (*lock)
			err = 0;
		else
			err = -ENOMEM;
		break;
	};

	return err;
}

#endif

static long pfp_admit_task(struct task_struct* tsk)
{
	if (task_cpu(tsk) == tsk->rt_param.task_params.cpu &&
#ifdef CONFIG_RELEASE_MASTER
	    /* don't allow tasks on release master CPU */
	    task_cpu(tsk) != remote_dom(task_cpu(tsk))->release_master &&
#endif
	    litmus_is_valid_fixed_prio(get_priority(tsk)))
		return 0;
	else
		return -EINVAL;
}

static long pfp_activate_plugin(void)
{
#if defined(CONFIG_RELEASE_MASTER) || defined(CONFIG_LITMUS_LOCKING)
	int cpu;
#endif

#ifdef CONFIG_RELEASE_MASTER
	for_each_online_cpu(cpu) {
		remote_dom(cpu)->release_master = atomic_read(&release_master_cpu);
	}
#endif

#ifdef CONFIG_LITMUS_LOCKING
	get_srp_prio = pfp_get_srp_prio;

	for_each_online_cpu(cpu) {
		init_waitqueue_head(&per_cpu(mpcpvs_vspin_wait, cpu));
		per_cpu(mpcpvs_vspin, cpu) = NULL;

		pcp_init_state(&per_cpu(pcp_state, cpu));
		pfp_doms[cpu] = remote_pfp(cpu);
	}

#endif

	return 0;
}


/*	Plugin object	*/
static struct sched_plugin pfp_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "P-FP",
	.tick			= pfp_tick,
	.task_new		= pfp_task_new,
	.complete_job		= complete_job,
	.task_exit		= pfp_task_exit,
	.schedule		= pfp_schedule,
	.task_wake_up		= pfp_task_wake_up,
	.task_block		= pfp_task_block,
	.admit_task		= pfp_admit_task,
	.activate_plugin	= pfp_activate_plugin,
#ifdef CONFIG_LITMUS_LOCKING
	.allocate_lock		= pfp_allocate_lock,
	.finish_switch		= pfp_finish_switch,
#endif
};


static int __init init_pfp(void)
{
	int i;

	/* We do not really want to support cpu hotplug, do we? ;)
	 * However, if we are so crazy to do so,
	 * we cannot use num_online_cpu()
	 */
	for (i = 0; i < num_online_cpus(); i++) {
		pfp_domain_init(remote_pfp(i), i);
	}
	return register_sched_plugin(&pfp_plugin);
}

module_init(init_pfp);

