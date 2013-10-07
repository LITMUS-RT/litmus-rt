/*
 * litmus.c -- Implementation of the LITMUS syscalls,
 *             the LITMUS intialization code,
 *             and the procfs interface..
 */
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/sysrq.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/reboot.h>
#include <linux/stop_machine.h>
#include <linux/sched/rt.h>

#include <litmus/litmus.h>
#include <litmus/bheap.h>
#include <litmus/trace.h>
#include <litmus/rt_domain.h>
#include <litmus/litmus_proc.h>
#include <litmus/sched_trace.h>

#ifdef CONFIG_SCHED_CPU_AFFINITY
#include <litmus/affinity.h>
#endif

#ifdef CONFIG_SCHED_LITMUS_TRACEPOINT
#define CREATE_TRACE_POINTS
#include <trace/events/litmus.h>
#endif

/* Number of RT tasks that exist in the system */
atomic_t rt_task_count 		= ATOMIC_INIT(0);

#ifdef CONFIG_RELEASE_MASTER
/* current master CPU for handling timer IRQs */
atomic_t release_master_cpu = ATOMIC_INIT(NO_CPU);
#endif

static struct kmem_cache * bheap_node_cache;
extern struct kmem_cache * release_heap_cache;

struct bheap_node* bheap_node_alloc(int gfp_flags)
{
	return kmem_cache_alloc(bheap_node_cache, gfp_flags);
}

void bheap_node_free(struct bheap_node* hn)
{
	kmem_cache_free(bheap_node_cache, hn);
}

struct release_heap* release_heap_alloc(int gfp_flags);
void release_heap_free(struct release_heap* rh);

/*
 * sys_set_task_rt_param
 * @pid: Pid of the task which scheduling parameters must be changed
 * @param: New real-time extension parameters such as the execution cost and
 *         period
 * Syscall for manipulating with task rt extension params
 * Returns EFAULT  if param is NULL.
 *         ESRCH   if pid is not corrsponding
 *	           to a valid task.
 *	   EINVAL  if either period or execution cost is <=0
 *	   EPERM   if pid is a real-time task
 *	   0       if success
 *
 * Only non-real-time tasks may be configured with this system call
 * to avoid races with the scheduler. In practice, this means that a
 * task's parameters must be set _before_ calling sys_prepare_rt_task()
 *
 * find_task_by_vpid() assumes that we are in the same namespace of the
 * target.
 */
asmlinkage long sys_set_rt_task_param(pid_t pid, struct rt_task __user * param)
{
	struct rt_task tp;
	struct task_struct *target;
	int retval = -EINVAL;

	printk("Setting up rt task parameters for process %d.\n", pid);

	if (pid < 0 || param == 0) {
		goto out;
	}
	if (copy_from_user(&tp, param, sizeof(tp))) {
		retval = -EFAULT;
		goto out;
	}

	/* Task search and manipulation must be protected */
	read_lock_irq(&tasklist_lock);
	if (!(target = find_task_by_vpid(pid))) {
		retval = -ESRCH;
		goto out_unlock;
	}

	if (is_realtime(target)) {
		/* The task is already a real-time task.
		 * We cannot not allow parameter changes at this point.
		 */
		retval = -EBUSY;
		goto out_unlock;
	}

	/* set relative deadline to be implicit if left unspecified */
	if (tp.relative_deadline == 0)
		tp.relative_deadline = tp.period;

	if (tp.exec_cost <= 0)
		goto out_unlock;
	if (tp.period <= 0)
		goto out_unlock;
	if (!cpu_online(tp.cpu))
		goto out_unlock;
	if (min(tp.relative_deadline, tp.period) < tp.exec_cost) /*density check*/
	{
		printk(KERN_INFO "litmus: real-time task %d rejected "
		       "because task density > 1.0\n", pid);
		goto out_unlock;
	}
	if (tp.cls != RT_CLASS_HARD &&
	    tp.cls != RT_CLASS_SOFT &&
	    tp.cls != RT_CLASS_BEST_EFFORT)
	{
		printk(KERN_INFO "litmus: real-time task %d rejected "
				 "because its class is invalid\n", pid);
		goto out_unlock;
	}
	if (tp.budget_policy != NO_ENFORCEMENT &&
	    tp.budget_policy != QUANTUM_ENFORCEMENT &&
	    tp.budget_policy != PRECISE_ENFORCEMENT)
	{
		printk(KERN_INFO "litmus: real-time task %d rejected "
		       "because unsupported budget enforcement policy "
		       "specified (%d)\n",
		       pid, tp.budget_policy);
		goto out_unlock;
	}

	target->rt_param.task_params = tp;

	retval = 0;
      out_unlock:
	read_unlock_irq(&tasklist_lock);
      out:
	return retval;
}

/*
 * Getter of task's RT params
 *   returns EINVAL if param or pid is NULL
 *   returns ESRCH  if pid does not correspond to a valid task
 *   returns EFAULT if copying of parameters has failed.
 *
 *   find_task_by_vpid() assumes that we are in the same namespace of the
 *   target.
 */
asmlinkage long sys_get_rt_task_param(pid_t pid, struct rt_task __user * param)
{
	int retval = -EINVAL;
	struct task_struct *source;
	struct rt_task lp;
	if (param == 0 || pid < 0)
		goto out;
	read_lock(&tasklist_lock);
	if (!(source = find_task_by_vpid(pid))) {
		retval = -ESRCH;
		goto out_unlock;
	}
	lp = source->rt_param.task_params;
	read_unlock(&tasklist_lock);
	/* Do copying outside the lock */
	retval =
	    copy_to_user(param, &lp, sizeof(lp)) ? -EFAULT : 0;
	return retval;
      out_unlock:
	read_unlock(&tasklist_lock);
      out:
	return retval;

}

/*
 *	This is the crucial function for periodic task implementation,
 *	It checks if a task is periodic, checks if such kind of sleep
 *	is permitted and calls plugin-specific sleep, which puts the
 *	task into a wait array.
 *	returns 0 on successful wakeup
 *	returns EPERM if current conditions do not permit such sleep
 *	returns EINVAL if current task is not able to go to sleep
 */
asmlinkage long sys_complete_job(void)
{
	int retval = -EPERM;
	if (!is_realtime(current)) {
		retval = -EINVAL;
		goto out;
	}
	/* Task with negative or zero period cannot sleep */
	if (get_rt_period(current) <= 0) {
		retval = -EINVAL;
		goto out;
	}
	/* The plugin has to put the task into an
	 * appropriate queue and call schedule
	 */
	retval = litmus->complete_job();
      out:
	return retval;
}

/*	This is an "improved" version of sys_complete_job that
 *      addresses the problem of unintentionally missing a job after
 *      an overrun.
 *
 *	returns 0 on successful wakeup
 *	returns EPERM if current conditions do not permit such sleep
 *	returns EINVAL if current task is not able to go to sleep
 */
asmlinkage long sys_wait_for_job_release(unsigned int job)
{
	int retval = -EPERM;
	if (!is_realtime(current)) {
		retval = -EINVAL;
		goto out;
	}

	/* Task with negative or zero period cannot sleep */
	if (get_rt_period(current) <= 0) {
		retval = -EINVAL;
		goto out;
	}

	retval = 0;

	/* first wait until we have "reached" the desired job
	 *
	 * This implementation has at least two problems:
	 *
	 * 1) It doesn't gracefully handle the wrap around of
	 *    job_no. Since LITMUS is a prototype, this is not much
	 *    of a problem right now.
	 *
	 * 2) It is theoretically racy if a job release occurs
	 *    between checking job_no and calling sleep_next_period().
	 *    A proper solution would requiring adding another callback
	 *    in the plugin structure and testing the condition with
	 *    interrupts disabled.
	 *
	 * FIXME: At least problem 2 should be taken care of eventually.
	 */
	while (!retval && job > current->rt_param.job_params.job_no)
		/* If the last job overran then job <= job_no and we
		 * don't send the task to sleep.
		 */
		retval = litmus->complete_job();
      out:
	return retval;
}

/*	This is a helper syscall to query the current job sequence number.
 *
 *	returns 0 on successful query
 *	returns EPERM if task is not a real-time task.
 *      returns EFAULT if &job is not a valid pointer.
 */
asmlinkage long sys_query_job_no(unsigned int __user *job)
{
	int retval = -EPERM;
	if (is_realtime(current))
		retval = put_user(current->rt_param.job_params.job_no, job);

	return retval;
}

/* sys_null_call() is only used for determining raw system call
 * overheads (kernel entry, kernel exit). It has no useful side effects.
 * If ts is non-NULL, then the current Feather-Trace time is recorded.
 */
asmlinkage long sys_null_call(cycles_t __user *ts)
{
	long ret = 0;
	cycles_t now;

	if (ts) {
		now = get_cycles();
		ret = put_user(now, ts);
	}

	return ret;
}

/* p is a real-time task. Re-init its state as a best-effort task. */
static void reinit_litmus_state(struct task_struct* p, int restore)
{
	struct rt_task  user_config = {};
	void*  ctrl_page     = NULL;

	if (restore) {
		/* Safe user-space provided configuration data.
		 * and allocated page. */
		user_config = p->rt_param.task_params;
		ctrl_page   = p->rt_param.ctrl_page;
	}

	/* We probably should not be inheriting any task's priority
	 * at this point in time.
	 */
	WARN_ON(p->rt_param.inh_task);

	/* Cleanup everything else. */
	memset(&p->rt_param, 0, sizeof(p->rt_param));

	/* Restore preserved fields. */
	if (restore) {
		p->rt_param.task_params = user_config;
		p->rt_param.ctrl_page   = ctrl_page;
	}
}

long litmus_admit_task(struct task_struct* tsk)
{
	long retval = 0;

	BUG_ON(is_realtime(tsk));

	tsk_rt(tsk)->heap_node = NULL;
	tsk_rt(tsk)->rel_heap = NULL;

	if (get_rt_relative_deadline(tsk) == 0 ||
	    get_exec_cost(tsk) >
			min(get_rt_relative_deadline(tsk), get_rt_period(tsk)) ) {
		TRACE_TASK(tsk,
			"litmus admit: invalid task parameters "
			"(e = %lu, p = %lu, d = %lu)\n",
			get_exec_cost(tsk), get_rt_period(tsk),
			get_rt_relative_deadline(tsk));
		retval = -EINVAL;
		goto out;
	}

	if (!cpu_online(get_partition(tsk))) {
		TRACE_TASK(tsk, "litmus admit: cpu %d is not online\n",
			   get_partition(tsk));
		retval = -EINVAL;
		goto out;
	}

	INIT_LIST_HEAD(&tsk_rt(tsk)->list);

	/* allocate heap node for this task */
	tsk_rt(tsk)->heap_node = bheap_node_alloc(GFP_ATOMIC);
	tsk_rt(tsk)->rel_heap = release_heap_alloc(GFP_ATOMIC);

	if (!tsk_rt(tsk)->heap_node || !tsk_rt(tsk)->rel_heap) {
		printk(KERN_WARNING "litmus: no more heap node memory!?\n");

		retval = -ENOMEM;
		goto out;
	} else {
		bheap_node_init(&tsk_rt(tsk)->heap_node, tsk);
	}

	preempt_disable();

	retval = litmus->admit_task(tsk);

	if (!retval) {
		sched_trace_task_name(tsk);
		sched_trace_task_param(tsk);
		atomic_inc(&rt_task_count);
	}

	preempt_enable();

out:
	if (retval) {
		if (tsk_rt(tsk)->heap_node)
			bheap_node_free(tsk_rt(tsk)->heap_node);
		if (tsk_rt(tsk)->rel_heap)
			release_heap_free(tsk_rt(tsk)->rel_heap);
	}
	return retval;
}

void litmus_clear_state(struct task_struct* tsk)
{
    BUG_ON(bheap_node_in_heap(tsk_rt(tsk)->heap_node));
    bheap_node_free(tsk_rt(tsk)->heap_node);
    release_heap_free(tsk_rt(tsk)->rel_heap);

    atomic_dec(&rt_task_count);
    reinit_litmus_state(tsk, 1);
}

void litmus_exit_task(struct task_struct* tsk)
{
	if (is_realtime(tsk)) {
		sched_trace_task_completion(tsk, 1);

		litmus->task_exit(tsk);
	}
}

static int do_plugin_switch(void *_plugin)
{
	int ret;
	struct sched_plugin* plugin = _plugin;

	/* don't switch if there are active real-time tasks */
	if (atomic_read(&rt_task_count) == 0) {
		ret = litmus->deactivate_plugin();
		if (0 != ret)
			goto out;
		ret = plugin->activate_plugin();
		if (0 != ret) {
			printk(KERN_INFO "Can't activate %s (%d).\n",
			       plugin->plugin_name, ret);
			plugin = &linux_sched_plugin;
		}
		printk(KERN_INFO "Switching to LITMUS^RT plugin %s.\n", plugin->plugin_name);
		litmus = plugin;
	} else
		ret = -EBUSY;
out:
	return ret;
}

/* Switching a plugin in use is tricky.
 * We must watch out that no real-time tasks exists
 * (and that none is created in parallel) and that the plugin is not
 * currently in use on any processor (in theory).
 */
int switch_sched_plugin(struct sched_plugin* plugin)
{
	BUG_ON(!plugin);

	if (atomic_read(&rt_task_count) == 0)
		return stop_machine(do_plugin_switch, plugin, NULL);
	else
		return -EBUSY;
}

/* Called upon fork.
 * p is the newly forked task.
 */
void litmus_fork(struct task_struct* p)
{
	if (is_realtime(p)) {
		/* clean out any litmus related state, don't preserve anything */
		reinit_litmus_state(p, 0);
		/* Don't let the child be a real-time task.  */
		p->sched_reset_on_fork = 1;
	} else
		/* non-rt tasks might have ctrl_page set */
		tsk_rt(p)->ctrl_page = NULL;

	/* od tables are never inherited across a fork */
	p->od_table = NULL;
}

/* Called upon execve().
 * current is doing the exec.
 * Don't let address space specific stuff leak.
 */
void litmus_exec(void)
{
	struct task_struct* p = current;

	if (is_realtime(p)) {
		WARN_ON(p->rt_param.inh_task);
		if (tsk_rt(p)->ctrl_page) {
			free_page((unsigned long) tsk_rt(p)->ctrl_page);
			tsk_rt(p)->ctrl_page = NULL;
		}
	}
}

void exit_litmus(struct task_struct *dead_tsk)
{
	/* We also allow non-RT tasks to
	 * allocate control pages to allow
	 * measurements with non-RT tasks.
	 * So check if we need to free the page
	 * in any case.
	 */
	if (tsk_rt(dead_tsk)->ctrl_page) {
		TRACE_TASK(dead_tsk,
			   "freeing ctrl_page %p\n",
			   tsk_rt(dead_tsk)->ctrl_page);
		free_page((unsigned long) tsk_rt(dead_tsk)->ctrl_page);
	}

	/* Tasks should not be real-time tasks any longer at this point. */
	BUG_ON(is_realtime(dead_tsk));
}


void litmus_do_exit(struct task_struct *exiting_tsk)
{
	/* This task called do_exit(), but is still a real-time task. To avoid
	 * complications later, we force it to be a non-real-time task now. */

	struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 };

	TRACE_TASK(exiting_tsk, "exiting, demoted to SCHED_FIFO\n");
	sched_setscheduler_nocheck(exiting_tsk, SCHED_FIFO, &param);
}

void litmus_dealloc(struct task_struct *tsk)
{
	/* tsk is no longer a real-time task */
	TRACE_TASK(tsk, "Deallocating real-time task data\n");
	litmus->task_cleanup(tsk);
	litmus_clear_state(tsk);
}

#ifdef CONFIG_MAGIC_SYSRQ
int sys_kill(int pid, int sig);

static void sysrq_handle_kill_rt_tasks(int key)
{
	struct task_struct *t;
	read_lock(&tasklist_lock);
	for_each_process(t) {
		if (is_realtime(t)) {
			sys_kill(t->pid, SIGKILL);
		}
	}
	read_unlock(&tasklist_lock);
}

static struct sysrq_key_op sysrq_kill_rt_tasks_op = {
	.handler	= sysrq_handle_kill_rt_tasks,
	.help_msg	= "quit-rt-tasks(X)",
	.action_msg	= "sent SIGKILL to all LITMUS^RT real-time tasks",
};
#endif

extern struct sched_plugin linux_sched_plugin;

static int litmus_shutdown_nb(struct notifier_block *unused1,
				unsigned long unused2, void *unused3)
{
	/* Attempt to switch back to regular Linux scheduling.
	 * Forces the active plugin to clean up.
	 */
	if (litmus != &linux_sched_plugin) {
		int ret = switch_sched_plugin(&linux_sched_plugin);
		if (ret) {
			printk("Auto-shutdown of active Litmus plugin failed.\n");
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block shutdown_notifier = {
	.notifier_call = litmus_shutdown_nb,
};

static int __init _init_litmus(void)
{
	/*      Common initializers,
	 *      mode change lock is used to enforce single mode change
	 *      operation.
	 */
	printk("Starting LITMUS^RT kernel\n");

	register_sched_plugin(&linux_sched_plugin);

	bheap_node_cache    = KMEM_CACHE(bheap_node, SLAB_PANIC);
	release_heap_cache = KMEM_CACHE(release_heap, SLAB_PANIC);

#ifdef CONFIG_MAGIC_SYSRQ
	/* offer some debugging help */
	if (!register_sysrq_key('x', &sysrq_kill_rt_tasks_op))
		printk("Registered kill rt tasks magic sysrq.\n");
	else
		printk("Could not register kill rt tasks magic sysrq.\n");
#endif

	init_litmus_proc();

#ifdef CONFIG_SCHED_CPU_AFFINITY
	init_topology();
#endif

	register_reboot_notifier(&shutdown_notifier);

	return 0;
}

static void _exit_litmus(void)
{
	unregister_reboot_notifier(&shutdown_notifier);

	exit_litmus_proc();
	kmem_cache_destroy(bheap_node_cache);
	kmem_cache_destroy(release_heap_cache);
}

module_init(_init_litmus);
module_exit(_exit_litmus);
