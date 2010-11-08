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


#ifdef CONFIG_SRP

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


#define system_ceiling(srp) list2prio(srp->ceiling.next)


#define UNDEF_SEM -2


/* struct for uniprocessor SRP "semaphore" */
struct srp_semaphore {
	struct srp_priority ceiling;
	struct task_struct* owner;
	int cpu; /* cpu associated with this "semaphore" and resource */
};

#define ceiling2sem(c) container_of(c, struct srp_semaphore, ceiling)

static int srp_exceeds_ceiling(struct task_struct* first,
			       struct srp* srp)
{
	return list_empty(&srp->ceiling) ||
	       get_rt_period(first) < system_ceiling(srp)->period ||
	       (get_rt_period(first) == system_ceiling(srp)->period &&
		first->pid < system_ceiling(srp)->pid) ||
		ceiling2sem(system_ceiling(srp))->owner == first;
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


static void* create_srp_semaphore(void)
{
	struct srp_semaphore* sem;

	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	INIT_LIST_HEAD(&sem->ceiling.list);
	sem->ceiling.period = 0;
	sem->cpu     = UNDEF_SEM;
	sem->owner   = NULL;
	atomic_inc(&srp_objects_in_use);
	return sem;
}

static noinline int open_srp_semaphore(struct od_table_entry* entry, void* __user arg)
{
	struct srp_semaphore* sem = (struct srp_semaphore*) entry->obj->obj;
	int ret = 0;
	struct task_struct* t = current;
	struct srp_priority t_prio;

	TRACE("opening SRP semaphore %p, cpu=%d\n", sem, sem->cpu);
	if (!srp_active())
		return -EBUSY;

	if (sem->cpu == UNDEF_SEM)
		sem->cpu = get_partition(t);
	else if (sem->cpu != get_partition(t))
		ret = -EPERM;

	if (ret == 0) {
		t_prio.period = get_rt_period(t);
		t_prio.pid    = t->pid;
		if (srp_higher_prio(&t_prio, &sem->ceiling)) {
			sem->ceiling.period = t_prio.period;
			sem->ceiling.pid    = t_prio.pid;
		}
	}

	return ret;
}

static void destroy_srp_semaphore(void* sem)
{
	/* XXX invariants */
	atomic_dec(&srp_objects_in_use);
	kfree(sem);
}

struct fdso_ops srp_sem_ops = {
	.create  = create_srp_semaphore,
	.open    = open_srp_semaphore,
	.destroy = destroy_srp_semaphore
};


static void do_srp_down(struct srp_semaphore* sem)
{
	/* Update ceiling. */
	srp_add_prio(&__get_cpu_var(srp), &sem->ceiling);
	WARN_ON(sem->owner != NULL);
	sem->owner = current;
	TRACE_CUR("acquired srp 0x%p\n", sem);
}

static void do_srp_up(struct srp_semaphore* sem)
{
	/* Determine new system priority ceiling for this CPU. */
	WARN_ON(!in_list(&sem->ceiling.list));
	if (in_list(&sem->ceiling.list))
		list_del(&sem->ceiling.list);

	sem->owner = NULL;

	/* Wake tasks on this CPU, if they exceed current ceiling. */
	TRACE_CUR("released srp 0x%p\n", sem);
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


#else

asmlinkage long sys_srp_down(int sem_od)
{
	return -ENOSYS;
}

asmlinkage long sys_srp_up(int sem_od)
{
	return -ENOSYS;
}

struct fdso_ops srp_sem_ops = {};

#endif
