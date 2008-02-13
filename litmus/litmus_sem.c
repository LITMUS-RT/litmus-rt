/*
 * PI semaphores and SRP implementations.
 * Much of the code here is borrowed from include/asm-i386/semaphore.h.
 *
 * NOTE: This implementation is very much a prototype and horribly insecure. It
 *       is intended to be a proof of concept, not a feature-complete solution.
 */

#include <asm/atomic.h>
#include <asm/semaphore.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>

#include <litmus/fdso.h>

#include <litmus/trace.h>

/* ************************************************************************** */
/*                          PRIORITY INHERITANCE                              */
/* ************************************************************************** */

static  void* create_pi_semaphore(void)
{
	struct pi_semaphore* sem;
	int i;

	sem = kmalloc(sizeof(struct pi_semaphore), GFP_KERNEL);
	if (!sem)
		return NULL;
	atomic_set(&sem->count, 1);
	sem->sleepers = 0;
	init_waitqueue_head(&sem->wait);
	sem->hp.task = NULL;
	sem->holder = NULL;
	for (i = 0; i < NR_CPUS; i++)
		sem->hp.cpu_task[i] = NULL;
	return sem;
}

static void destroy_pi_semaphore(void* sem)
{
	/* XXX assert invariants */
	kfree(sem);
}

struct fdso_ops pi_sem_ops = {
	.create  = create_pi_semaphore,
	.destroy = destroy_pi_semaphore
};

struct wq_pair {
	struct task_struct*  tsk;
	struct pi_semaphore* sem;
};

static int rt_pi_wake_up(wait_queue_t *wait, unsigned mode, int sync,
			   void *key)
{
	struct wq_pair* wqp   = (struct wq_pair*) wait->private;
	set_rt_flags(wqp->tsk, RT_F_EXIT_SEM);
	litmus->inherit_priority(wqp->sem, wqp->tsk);
	TRACE_TASK(wqp->tsk,
		   "woken up by rt_pi_wake_up() (RT_F_SEM_EXIT, PI)\n");
	/* point to task for default_wake_function() */
	wait->private = wqp->tsk;
	default_wake_function(wait, mode, sync, key);

	/* Always return true since we know that if we encountered a task
	 * that was already running the wake_up raced with the schedule in
	 * rt_pi_down(). In that case the task in rt_pi_down() will be scheduled
	 * immediately and own the lock. We must not wake up another task in
	 * any case.
	 */
	return 1;
}

/* caller is responsible for locking */
int edf_set_hp_task(struct pi_semaphore *sem)
{
	struct list_head	*tmp, *next;
	struct task_struct 	*queued;
	int ret = 0;

	sem->hp.task = NULL;
	list_for_each_safe(tmp, next, &sem->wait.task_list) {
		queued  = ((struct wq_pair*)
			list_entry(tmp, wait_queue_t,
				   task_list)->private)->tsk;

		/* Compare task prios, find high prio task. */
		if (edf_higher_prio(queued, sem->hp.task)) {
			sem->hp.task = queued;
			ret = 1;
		}
	}
	return ret;
}

/* caller is responsible for locking */
int edf_set_hp_cpu_task(struct pi_semaphore *sem, int cpu)
{
	struct list_head	*tmp, *next;
	struct task_struct 	*queued;
	int ret = 0;

	sem->hp.cpu_task[cpu] = NULL;
	list_for_each_safe(tmp, next, &sem->wait.task_list) {
		queued  = ((struct wq_pair*)
			list_entry(tmp, wait_queue_t,
				   task_list)->private)->tsk;

		/* Compare task prios, find high prio task. */
		if (get_partition(queued) == cpu &&
		    edf_higher_prio(queued, sem->hp.cpu_task[cpu])) {
			sem->hp.cpu_task[cpu] = queued;
			ret = 1;
		}
	}
	return ret;
}

int do_pi_down(struct pi_semaphore* sem)
{
	unsigned long flags;
	struct task_struct *tsk = current;
	struct wq_pair pair;
	int suspended = 1;
	wait_queue_t wait = {
		.private = &pair,
		.func    = rt_pi_wake_up,
		.task_list = {NULL, NULL}
	};

	pair.tsk = tsk;
	pair.sem = sem;
	spin_lock_irqsave(&sem->wait.lock, flags);

	if (atomic_dec_return(&sem->count) < 0 ||
	    waitqueue_active(&sem->wait)) {
		/* we need to suspend */
		tsk->state = TASK_UNINTERRUPTIBLE;
		add_wait_queue_exclusive_locked(&sem->wait, &wait);

		TRACE_CUR("suspends on PI lock %p\n", sem);
		litmus->pi_block(sem, tsk);

		/* release lock before sleeping */
		spin_unlock_irqrestore(&sem->wait.lock, flags);

		TS_PI_DOWN_END;
		preempt_enable_no_resched();


		/* we depend on the FIFO order
		 * Thus, we don't need to recheck when we wake up, we
		 * are guaranteed to have the lock since there is only one
		 * wake up per release
		 */
		schedule();

		TRACE_CUR("woke up, now owns PI lock %p\n", sem);

		/* try_to_wake_up() set our state to TASK_RUNNING,
		 * all we need to do is to remove our wait queue entry
		 */
		remove_wait_queue(&sem->wait, &wait);
	} else {
		/* no priority inheritance necessary, since there are no queued
		 * tasks.
		 */
		suspended = 0;
		TRACE_CUR("acquired PI lock %p, no contention\n", sem);
		sem->holder  = tsk;
		sem->hp.task = tsk;
		litmus->inherit_priority(sem, tsk);
		spin_unlock_irqrestore(&sem->wait.lock, flags);
	}
	return suspended;
}

void do_pi_up(struct pi_semaphore* sem)
{
	unsigned long flags;

	spin_lock_irqsave(&sem->wait.lock, flags);

	TRACE_CUR("releases PI lock %p\n", sem);
	litmus->return_priority(sem);
	sem->holder = NULL;
	if (atomic_inc_return(&sem->count) < 1)
		/* there is a task queued */
		wake_up_locked(&sem->wait);

	spin_unlock_irqrestore(&sem->wait.lock, flags);
}

asmlinkage long sys_pi_down(int sem_od)
{
	long ret = 0;
	struct pi_semaphore * sem;
	int suspended = 0;

	preempt_disable();
	TS_PI_DOWN_START;

	sem = lookup_pi_sem(sem_od);
	if (sem)
		suspended = do_pi_down(sem);
	else
		ret = -EINVAL;

	if (!suspended) {
		TS_PI_DOWN_END;
		preempt_enable();
	}

	return ret;
}

asmlinkage long sys_pi_up(int sem_od)
{
	long ret = 0;
	struct pi_semaphore * sem;

	preempt_disable();
	TS_PI_UP_START;

	sem = lookup_pi_sem(sem_od);
	if (sem)
		do_pi_up(sem);
	else
		ret = -EINVAL;


	TS_PI_UP_END;
	preempt_enable();

	return ret;
}

/* Clear wait queue and wakeup waiting tasks, and free semaphore. */
/*
asmlinkage long sys_pi_sema_free(int sem_id)
{
        struct list_head *tmp, *next;
	unsigned long flags;

        if (sem_id < 0 || sem_id >= MAX_PI_SEMAPHORES)
		return -EINVAL;

	if (!pi_sems[sem_id].used)
		return -EINVAL;

	spin_lock_irqsave(&pi_sems[sem_id].wait.lock, flags);
	if (waitqueue_active(&pi_sems[sem_id].wait)) {
		list_for_each_safe(tmp, next,
				   &pi_sems[sem_id].wait.task_list) {
			wait_queue_t *curr = list_entry(tmp, wait_queue_t,
					                task_list);
			list_del(tmp);
			set_rt_flags((struct task_struct*)curr->private,
				     RT_F_EXIT_SEM);
			curr->func(curr,
				   TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,
				   0, NULL);
		}
	}

	spin_unlock_irqrestore(&pi_sems[sem_id].wait.lock, flags);
	pi_sems[sem_id].used = 0;

	return 0;
}
*/



/* ************************************************************************** */
/*                          STACK RESOURCE POLICY                             */
/* ************************************************************************** */


struct srp_priority {
	struct list_head	list;
        unsigned int 		period;
	pid_t			pid;
};

#define list2prio(l) list_entry(l, struct srp_priority, list)

/* SRP task priority comparison function. Smaller periods have highest
 * priority, tie-break is PID. Special case: period == 0 <=> no priority
 */
static int srp_higher_prio(struct srp_priority* first,
			   struct srp_priority* second)
{
	if (!first->period)
		return 0;
	else
		return  !second->period ||
			first->period < second->period || (
			first->period == second->period &&
			first->pid < second->pid);
}

struct srp {
	struct list_head	ceiling;
	wait_queue_head_t	ceiling_blocked;
};


DEFINE_PER_CPU(struct srp, srp);

#define system_ceiling(srp) list2prio(srp->ceiling.next)

static int srp_exceeds_ceiling(struct task_struct* first,
			       struct srp* srp)
{
	return list_empty(&srp->ceiling) ||
	       get_rt_period(first) < system_ceiling(srp)->period ||
	       (get_rt_period(first) == system_ceiling(srp)->period &&
		first->pid < system_ceiling(srp)->pid);
}

static void srp_add_prio(struct srp* srp, struct srp_priority* prio)
{
	struct list_head *pos;
	if (in_list(&prio->list)) {
		TRACE_CUR("WARNING: SRP violation detected, prio is already in "
			  "ceiling list!\n");
		return;
	}
	list_for_each(pos, &srp->ceiling)
		if (unlikely(srp_higher_prio(prio, list2prio(pos)))) {
			__list_add(&prio->list, pos->prev, pos);
			return;
		}

	list_add_tail(&prio->list, &srp->ceiling);
}

/* struct for uniprocessor SRP "semaphore" */
struct srp_semaphore {
	struct srp_priority ceiling;
	int cpu; /* cpu associated with this "semaphore" and resource */
	int claimed; /* is the resource claimed (ceiling should be used)? */
};


static void* create_srp_semaphore(void)
{
	struct srp_semaphore* sem;

	if (!is_realtime(current))
		/* XXX log error */
		return NULL;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	INIT_LIST_HEAD(&sem->ceiling.list);
	sem->ceiling.period = 0;
	sem->claimed = 0;
	sem->cpu     = get_partition(current);
	return sem;
}

static void destroy_srp_semaphore(void* sem)
{
	/* XXX invariants */
	kfree(sem);
}

struct fdso_ops srp_sem_ops = {
	.create  = create_srp_semaphore,
	.destroy = destroy_srp_semaphore
};

/* Initialize SRP semaphores at boot time. */
static int __init srp_sema_boot_init(void)
{
	int i;

	printk("Initializing SRP per-CPU ceilings...");
	for (i = 0; i < NR_CPUS; i++) {
		init_waitqueue_head(&per_cpu(srp, i).ceiling_blocked);
		INIT_LIST_HEAD(&per_cpu(srp, i).ceiling);
	}
	printk(" done!\n");

	return 0;
}
__initcall(srp_sema_boot_init);


void do_srp_down(struct srp_semaphore* sem)
{
	/* claim... */
        sem->claimed = 1;
	/* ...and update ceiling */
	srp_add_prio(&__get_cpu_var(srp), &sem->ceiling);
}

void do_srp_up(struct srp_semaphore* sem)
{
	sem->claimed = 0;

	/* Determine new system priority ceiling for this CPU. */
	if (in_list(&sem->ceiling.list))
		list_del(&sem->ceiling.list);
	else
		TRACE_CUR("WARNING: SRP violation detected, prio not in ceiling"
			  " list!\n");

	/* Wake tasks on this CPU, if they exceed current ceiling. */
	wake_up_all(&__get_cpu_var(srp).ceiling_blocked);
}

/* Adjust the system-wide priority ceiling if resource is claimed. */
asmlinkage long sys_srp_down(int sem_od)
{
	int cpu;
	int ret = -EINVAL;
	struct srp_semaphore* sem;

	/* disabling preemptions is sufficient protection since
	 * SRP is strictly per CPU and we don't interfere with any
	 * interrupt handlers
	 */
	preempt_disable();
	TS_SRP_DOWN_START;

	cpu = smp_processor_id();
	sem = lookup_srp_sem(sem_od);
	if (sem && sem->cpu == cpu) {
		do_srp_down(sem);
		ret = 0;
	}

	TS_SRP_DOWN_END;
	preempt_enable();
	return ret;
}

/* Adjust the system-wide priority ceiling if resource is freed. */
asmlinkage long sys_srp_up(int sem_od)
{
	int cpu;
	int ret = -EINVAL;
	struct srp_semaphore* sem;

	preempt_disable();
	TS_SRP_UP_START;

	cpu = smp_processor_id();
	sem = lookup_srp_sem(sem_od);

	if (sem && sem->cpu == cpu) {
		do_srp_up(sem);
		ret = 0;
	}

	TS_SRP_UP_END;
	preempt_enable();
	return ret;
}

/* Indicate that task will use a resource associated with a given
 * semaphore. Should be done *a priori* before RT task system is
 * executed, so this does *not* update the system priority
 * ceiling! (The ceiling would be meaningless anyway, as the SRP
 * breaks without this a priori knowledge.)
 */
asmlinkage long sys_reg_task_srp_sem(int sem_od)
{
	/*
	 * FIXME: This whole concept is rather brittle!
	 *        There must be a better solution. Maybe register on
	 *        first reference?
	 */

	struct task_struct *t = current;
	struct srp_priority t_prio;
	struct srp_semaphore* sem;

	sem = lookup_srp_sem(sem_od);

	if (!sem)
		return -EINVAL;

	if (!is_realtime(t))
		return -EPERM;

	if (sem->cpu != get_partition(t))
		return -EINVAL;

	preempt_disable();
	t->rt_param.subject_to_srp = 1;
	t_prio.period = get_rt_period(t);
	t_prio.pid    = t->pid;
	if (srp_higher_prio(&t_prio, &sem->ceiling)) {
		sem->ceiling.period = t_prio.period;
		sem->ceiling.pid    = t_prio.pid;
	}

	preempt_enable();

	return 0;
}

static int srp_wake_up(wait_queue_t *wait, unsigned mode, int sync,
		       void *key)
{
	int cpu = smp_processor_id();
	struct task_struct *tsk = wait->private;
	if (cpu != get_partition(tsk))
		TRACE_TASK(tsk, "srp_wake_up on wrong cpu, partition is %d\b",
			   get_partition(tsk));
	else if (srp_exceeds_ceiling(tsk, &__get_cpu_var(srp)))
		return default_wake_function(wait, mode, sync, key);
	return 0;
}


/* Wait for current task priority to exceed system-wide priority ceiling.
 * Can be used to determine when it is safe to run a job after its release.
 */
void srp_ceiling_block(void)
{
	struct task_struct *tsk = current;
	wait_queue_t wait = {
		.private   = tsk,
		.func      = srp_wake_up,
		.task_list = {NULL, NULL}
	};

	preempt_disable();
	if (!srp_exceeds_ceiling(tsk, &__get_cpu_var(srp))) {
		tsk->state = TASK_UNINTERRUPTIBLE;
		add_wait_queue(&__get_cpu_var(srp).ceiling_blocked, &wait);
		TRACE_CUR("is priority ceiling blocked.\n");
		preempt_enable_no_resched();
		schedule();
		/* Access to CPU var must occur with preemptions disabled,
		 * otherwise Linux debug code complains loudly, even if it is
		 * ok here.
		 */
		preempt_disable();
		TRACE_CUR("finally exceeds system ceiling.\n");
		remove_wait_queue(&__get_cpu_var(srp).ceiling_blocked, &wait);
		preempt_enable();
	} else {
		TRACE_CUR("is not priority ceiling blocked\n");
		preempt_enable();
	}
}

/* ************************************************************************** */



