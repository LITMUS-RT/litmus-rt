/* ************************************************************************** */
/*                          STACK RESOURCE POLICY                             */
/* ************************************************************************** */

#include <asm/atomic.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/fdso.h>
#include <litmus/trace.h>


#ifdef CONFIG_LITMUS_LOCKING

#include <litmus/srp.h>

srp_prioritization_t get_srp_prio;

struct srp {
	struct list_head	ceiling;
	wait_queue_head_t	ceiling_blocked;
};
#define system_ceiling(srp) list2prio(srp->ceiling.next)
#define ceiling2sem(c) container_of(c, struct srp_semaphore, ceiling)

#define UNDEF_SEM -2

atomic_t srp_objects_in_use = ATOMIC_INIT(0);

DEFINE_PER_CPU(struct srp, srp);

/* Initialize SRP semaphores at boot time. */
static int __init srp_init(void)
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
module_init(srp_init);

/* SRP task priority comparison function. Smaller numeric values have higher
 * priority, tie-break is PID. Special case: priority == 0 <=> no priority
 */
static int srp_higher_prio(struct srp_priority* first,
			   struct srp_priority* second)
{
	if (!first->priority)
		return 0;
	else
		return  !second->priority ||
			first->priority < second->priority || (
			first->priority == second->priority &&
			first->pid < second->pid);
}


static int srp_exceeds_ceiling(struct task_struct* first,
			       struct srp* srp)
{
	struct srp_priority prio;

	if (list_empty(&srp->ceiling))
		return 1;
	else {
		prio.pid = first->pid;
		prio.priority = get_srp_prio(first);
		return srp_higher_prio(&prio, system_ceiling(srp)) ||
			ceiling2sem(system_ceiling(srp))->owner == first;
	}
}

static void srp_add_prio(struct srp* srp, struct srp_priority* prio)
{
	struct list_head *pos;
	if (in_list(&prio->list)) {
		printk(KERN_CRIT "WARNING: SRP violation detected, prio is already in "
		       "ceiling list! cpu=%d, srp=%p\n", smp_processor_id(), ceiling2sem(prio));
		return;
	}
	list_for_each(pos, &srp->ceiling)
		if (unlikely(srp_higher_prio(prio, list2prio(pos)))) {
			__list_add(&prio->list, pos->prev, pos);
			return;
		}

	list_add_tail(&prio->list, &srp->ceiling);
}


static int lock_srp_semaphore(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct srp_semaphore* sem = container_of(l, struct srp_semaphore, litmus_lock);

	if (!is_realtime(t))
		return -EPERM;

	/* prevent acquisition of local locks in global critical sections */
	if (tsk_rt(t)->num_locks_held)
		return -EBUSY;

	preempt_disable();

	/* Update ceiling. */
	srp_add_prio(&__get_cpu_var(srp), &sem->ceiling);

	/* SRP invariant: all resources available */
	BUG_ON(sem->owner != NULL);

	sem->owner = t;
	TRACE_CUR("acquired srp 0x%p\n", sem);

	tsk_rt(t)->num_local_locks_held++;

	preempt_enable();

	return 0;
}

static int unlock_srp_semaphore(struct litmus_lock* l)
{
	struct task_struct* t = current;
	struct srp_semaphore* sem = container_of(l, struct srp_semaphore, litmus_lock);
	int err = 0;

	preempt_disable();

	if (sem->owner != t) {
		err = -EINVAL;
	} else {
		/* The current owner should be executing on the correct CPU.
		 *
		 * FIXME: if the owner transitioned out of RT mode or is
		 * exiting, then we it might have already been migrated away by
		 * the best-effort scheduler and we just have to deal with
		 * it. This is currently not supported. */
		BUG_ON(sem->cpu != smp_processor_id());

		/* Determine new system priority ceiling for this CPU. */
		BUG_ON(!in_list(&sem->ceiling.list));

		list_del(&sem->ceiling.list);
		sem->owner = NULL;

		/* Wake tasks on this CPU, if they exceed current ceiling. */
		TRACE_CUR("released srp 0x%p\n", sem);
		wake_up_all(&__get_cpu_var(srp).ceiling_blocked);

		tsk_rt(t)->num_local_locks_held--;
	}

	preempt_enable();
	return err;
}

static int open_srp_semaphore(struct litmus_lock* l, void* __user arg)
{
	struct srp_semaphore* sem = container_of(l, struct srp_semaphore, litmus_lock);
	int err = 0;
	struct task_struct* t = current;
	struct srp_priority t_prio;

	if (!is_realtime(t))
		return -EPERM;

	TRACE_CUR("opening SRP semaphore %p, cpu=%d\n", sem, sem->cpu);

	preempt_disable();

	if (sem->owner != NULL)
		err = -EBUSY;

	if (err == 0) {
		if (sem->cpu == UNDEF_SEM)
			sem->cpu = get_partition(t);
		else if (sem->cpu != get_partition(t))
			err = -EPERM;
	}

	if (err == 0) {
		t_prio.priority = get_srp_prio(t);
		t_prio.pid      = t->pid;
		if (srp_higher_prio(&t_prio, &sem->ceiling)) {
			sem->ceiling.priority = t_prio.priority;
			sem->ceiling.pid      = t_prio.pid;
		}
	}

	preempt_enable();

	return err;
}

static int close_srp_semaphore(struct litmus_lock* l)
{
	struct srp_semaphore* sem = container_of(l, struct srp_semaphore, litmus_lock);
	int err = 0;

	preempt_disable();

	if (sem->owner == current)
		unlock_srp_semaphore(l);

	preempt_enable();

	return err;
}

static void deallocate_srp_semaphore(struct litmus_lock* l)
{
	struct srp_semaphore* sem = container_of(l, struct srp_semaphore, litmus_lock);
	atomic_dec(&srp_objects_in_use);
	kfree(sem);
}

static struct litmus_lock_ops srp_lock_ops = {
	.open   = open_srp_semaphore,
	.close  = close_srp_semaphore,
	.lock   = lock_srp_semaphore,
	.unlock = unlock_srp_semaphore,
	.deallocate = deallocate_srp_semaphore,
};

struct srp_semaphore* allocate_srp_semaphore(void)
{
	struct srp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	INIT_LIST_HEAD(&sem->ceiling.list);
	sem->ceiling.priority = 0;
	sem->cpu     = UNDEF_SEM;
	sem->owner   = NULL;

	sem->litmus_lock.ops = &srp_lock_ops;

	atomic_inc(&srp_objects_in_use);
	return sem;
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

static void do_ceiling_block(struct task_struct *tsk)
{
	wait_queue_t wait = {
		.private   = tsk,
		.func      = srp_wake_up,
		.task_list = {NULL, NULL}
	};

	tsk->state = TASK_UNINTERRUPTIBLE;
	add_wait_queue(&__get_cpu_var(srp).ceiling_blocked, &wait);
	tsk->rt_param.srp_non_recurse = 1;
	preempt_enable_no_resched();
	schedule();
	preempt_disable();
	tsk->rt_param.srp_non_recurse = 0;
	remove_wait_queue(&__get_cpu_var(srp).ceiling_blocked, &wait);
}

/* Wait for current task priority to exceed system-wide priority ceiling.
 * FIXME: the hotpath should be inline.
 */
void srp_ceiling_block(void)
{
	struct task_struct *tsk = current;

	/* Only applies to real-time tasks, but optimize for RT tasks. */
	if (unlikely(!is_realtime(tsk)))
		return;

	/* Avoid recursive ceiling blocking. */
	if (unlikely(tsk->rt_param.srp_non_recurse))
		return;

	/* Bail out early if there aren't any SRP resources around. */
	if (likely(!atomic_read(&srp_objects_in_use)))
		return;

	preempt_disable();
	if (!srp_exceeds_ceiling(tsk, &__get_cpu_var(srp))) {
		TRACE_CUR("is priority ceiling blocked.\n");
		while (!srp_exceeds_ceiling(tsk, &__get_cpu_var(srp)))
			do_ceiling_block(tsk);
		TRACE_CUR("finally exceeds system ceiling.\n");
	} else
		TRACE_CUR("is not priority ceiling blocked\n");
	preempt_enable();
}

#endif
