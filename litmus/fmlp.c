/*
 * FMLP implementation.
 * Much of the code here is borrowed from include/asm-i386/semaphore.h
 */

#include <asm/atomic.h>

#include <linux/semaphore.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>

#include <litmus/fdso.h>

#include <litmus/trace.h>

#ifdef CONFIG_FMLP

static  void* create_fmlp_semaphore(void)
{
	struct pi_semaphore* sem;
	int i;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
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

static int open_fmlp_semaphore(struct od_table_entry* entry, void* __user arg)
{
	if (!fmlp_active())
		return -EBUSY;
	return 0;
}

static void destroy_fmlp_semaphore(void* sem)
{
	/* XXX assert invariants */
	kfree(sem);
}

struct fdso_ops fmlp_sem_ops = {
	.create  = create_fmlp_semaphore,
	.open    = open_fmlp_semaphore,
	.destroy = destroy_fmlp_semaphore
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

static int do_fmlp_down(struct pi_semaphore* sem)
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
		__add_wait_queue_tail_exclusive(&sem->wait, &wait);

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

		/* don't know if we're global or partitioned. */
		sem->hp.task = tsk;
		sem->hp.cpu_task[get_partition(tsk)] = tsk;

		litmus->inherit_priority(sem, tsk);
		spin_unlock_irqrestore(&sem->wait.lock, flags);
	}
	return suspended;
}

static void do_fmlp_up(struct pi_semaphore* sem)
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

asmlinkage long sys_fmlp_down(int sem_od)
{
	long ret = 0;
	struct pi_semaphore * sem;
	int suspended = 0;

	preempt_disable();
	TS_PI_DOWN_START;

	sem = lookup_fmlp_sem(sem_od);
	if (sem)
		suspended = do_fmlp_down(sem);
	else
		ret = -EINVAL;

	if (!suspended) {
		TS_PI_DOWN_END;
		preempt_enable();
	}

	return ret;
}

asmlinkage long sys_fmlp_up(int sem_od)
{
	long ret = 0;
	struct pi_semaphore * sem;

	preempt_disable();
	TS_PI_UP_START;

	sem = lookup_fmlp_sem(sem_od);
	if (sem)
		do_fmlp_up(sem);
	else
		ret = -EINVAL;


	TS_PI_UP_END;
	preempt_enable();

	return ret;
}

#else

struct fdso_ops fmlp_sem_ops = {};

asmlinkage long sys_fmlp_down(int sem_od)
{
	return -ENOSYS;
}

asmlinkage long sys_fmlp_up(int sem_od)
{
	return -ENOSYS;
}

#endif
