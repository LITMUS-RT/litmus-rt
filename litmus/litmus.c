/* litmus.c -- Implementation of the LITMUS syscalls, the LITMUS intialization code,
 *             and the procfs interface..
 */
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include <linux/sysrq.h>

#include <linux/module.h>
#include <linux/proc_fs.h>


#include <litmus/litmus.h>
#include <linux/sched.h>
#include <litmus/sched_plugin.h>

#include <litmus/trace.h>

/* Number of RT tasks that exist in the system */
atomic_t rt_task_count 		= ATOMIC_INIT(0);
static DEFINE_SPINLOCK(task_transition_lock);

/* To send signals from the scheduler
 * Must drop locks first.
 */
static LIST_HEAD(sched_sig_list);
static DEFINE_SPINLOCK(sched_sig_list_lock);

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

	/*      Task search and manipulation must be protected */
	read_lock_irq(&tasklist_lock);
	if (!(target = find_task_by_pid(pid))) {
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

	if (tp.exec_cost <= 0)
		goto out_unlock;
	if (tp.period <= 0)
		goto out_unlock;
	if (!cpu_online(tp.cpu))
		goto out_unlock;
	if (tp.period < tp.exec_cost)
	{
		printk(KERN_INFO "litmus: real-time task %d rejected "
		       "because wcet > period\n", pid);
		goto out_unlock;
	}

	target->rt_param.task_params = tp;

	retval = 0;
      out_unlock:
	read_unlock_irq(&tasklist_lock);
      out:
	return retval;
}

/*	Getter of task's RT params
 *	returns EINVAL if param or pid is NULL
 *	returns ESRCH  if pid does not correspond to a valid task
 *	returns EFAULT if copying of parameters has failed.
 */
asmlinkage long sys_get_rt_task_param(pid_t pid, struct rt_task __user * param)
{
	int retval = -EINVAL;
	struct task_struct *source;
	struct rt_task lp;
	if (param == 0 || pid < 0)
		goto out;
	read_lock(&tasklist_lock);
	if (!(source = find_task_by_pid(pid))) {
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

/* implemented in kernel/litmus_sem.c */
void srp_ceiling_block(void);

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
	if (!retval && is_subject_to_srp(current))
		srp_ceiling_block();
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

	/* We still have to honor the SRP after the actual release.
	 */
	if (!retval && is_subject_to_srp(current))
		srp_ceiling_block();
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

struct sched_sig {
	struct list_head 	list;
	struct task_struct*	task;
	unsigned int		signal:31;
	int			force:1;
};

static void __scheduler_signal(struct task_struct *t, unsigned int signo,
			       int force)
{
	struct sched_sig* sig;

	sig = kmalloc(GFP_ATOMIC, sizeof(struct sched_sig));
	if (!sig) {
		TRACE_TASK(t, "dropping signal: %u\n", t);
		return;
	}

	spin_lock(&sched_sig_list_lock);

	sig->signal = signo;
	sig->force  = force;
	sig->task   = t;
	get_task_struct(t);
	list_add(&sig->list, &sched_sig_list);

	spin_unlock(&sched_sig_list_lock);
}

void scheduler_signal(struct task_struct *t, unsigned int signo)
{
	__scheduler_signal(t, signo, 0);
}

void force_scheduler_signal(struct task_struct *t, unsigned int signo)
{
	__scheduler_signal(t, signo, 1);
}

/* FIXME: get rid of the locking and do this on a per-processor basis */
void send_scheduler_signals(void)
{
	unsigned long flags;
	struct list_head *p, *extra;
	struct siginfo info;
	struct sched_sig* sig;
	struct task_struct* t;
	struct list_head claimed;

	if (spin_trylock_irqsave(&sched_sig_list_lock, flags)) {
		if (list_empty(&sched_sig_list))
			p = NULL;
		else {
			p = sched_sig_list.next;
			list_del(&sched_sig_list);
			INIT_LIST_HEAD(&sched_sig_list);
		}
		spin_unlock_irqrestore(&sched_sig_list_lock, flags);

		/* abort if there are no signals */
		if (!p)
			return;

		/* take signal list we just obtained */
		list_add(&claimed, p);

		list_for_each_safe(p, extra, &claimed) {
			list_del(p);
			sig = list_entry(p, struct sched_sig, list);
			t = sig->task;
			info.si_signo = sig->signal;
			info.si_errno = 0;
			info.si_code  = SI_KERNEL;
			info.si_pid   = 1;
			info.si_uid   = 0;
			TRACE("sending signal %d to %d\n", info.si_signo,
			      t->pid);
			if (sig->force)
				force_sig_info(sig->signal, &info, t);
			else
				send_sig_info(sig->signal, &info, t);
			put_task_struct(t);
			kfree(sig);
		}
	}

}

static inline void np_mem_error(struct task_struct* t, const char* reason)
{
	if (t->state != TASK_DEAD && !(t->flags & PF_EXITING)) {
		TRACE("np section: %s => %s/%d killed\n",
		      reason, t->comm, t->pid);
		force_scheduler_signal(t, SIGKILL);
	}
}

/*	sys_register_np_flag() allows real-time tasks to register an
 *	np section indicator.
 *	returns 0      if the flag was successfully registered
 *	returns EINVAL if current task is not a real-time task
 *	returns EFAULT if *flag couldn't be written
 */
asmlinkage long sys_register_np_flag(short __user *flag)
{
	int retval = -EINVAL;
	short test_val = RT_PREEMPTIVE;

	/* avoid races with the scheduler */
	preempt_disable();
	TRACE("reg_np_flag(%p) for %s/%d\n", flag,
	      current->comm, current->pid);

	/* Let's first try to write to the address.
	 * That way it is initialized and any bugs
	 * involving dangling pointers will caught
	 * early.
	 * NULL indicates disabling np section support
	 * and should not be tested.
	 */
	if (flag)
	  retval = poke_kernel_address(test_val, flag);
	else
	  retval = 0;
	TRACE("reg_np_flag: retval=%d\n", retval);
	if (unlikely(0 != retval))
		np_mem_error(current, "np flag: not writable");
	else
	  /* the pointer is ok */
	  current->rt_param.np_flag = flag;

	preempt_enable();
	return retval;
}


void request_exit_np(struct task_struct *t)
{
	int ret;
	short flag;

	/* We can only do this if t is actually currently scheduled on this CPU
	 * because otherwise we are in the wrong address space. Thus make sure
	 * to check.
	 */
        BUG_ON(t != current);

	if (unlikely(!is_realtime(t) || !t->rt_param.np_flag)) {
		TRACE_TASK(t, "request_exit_np(): BAD TASK!\n");
		return;
	}

	flag = RT_EXIT_NP_REQUESTED;
	ret  = poke_kernel_address(flag, t->rt_param.np_flag + 1);
	TRACE("request_exit_np(%s/%d)\n", t->comm, t->pid);
	if (unlikely(0 != ret))
		np_mem_error(current, "request_exit_np(): flag not writable");

}


int is_np(struct task_struct* t)
{
	int ret;
	unsigned short flag = 0x5858; /* = XX, looks nicer in debug*/

        BUG_ON(t != current);

	if (unlikely(t->rt_param.kernel_np))
		return 1;
	else if (unlikely(t->rt_param.np_flag == NULL) ||
		 t->flags & PF_EXITING ||
		 t->state == TASK_DEAD)
		return 0;
	else {
		/* This is the tricky part. The process has registered a
		 * non-preemptive section marker. We now need to check whether
		 * it is set to to NON_PREEMPTIVE. Along the way we could
		 * discover that the pointer points to an unmapped region (=>
		 * kill the task) or that the location contains some garbage
		 * value (=> also kill the task). Killing the task in any case
		 * forces userspace to play nicely. Any bugs will be discovered
		 * immediately.
		 */
		ret = probe_kernel_address(t->rt_param.np_flag, flag);
		if (0 == ret && (flag == RT_NON_PREEMPTIVE ||
				 flag == RT_PREEMPTIVE))
		return flag != RT_PREEMPTIVE;
		else {
			/* either we could not read from the address or
			 * it contained garbage => kill the process
			 * FIXME: Should we cause a SEGFAULT instead?
			 */
			TRACE("is_np: ret=%d flag=%c%c (%x)\n", ret,
			      flag & 0xff, (flag >> 8) & 0xff, flag);
			np_mem_error(t, "is_np() could not read");
			return 0;
		}
	}
}

/*
 *	sys_exit_np() allows real-time tasks to signal that it left a
 *      non-preemptable section. It will be called after the kernel requested a
 *      callback in the preemption indicator flag.
 *	returns 0      if the signal was valid and processed.
 *	returns EINVAL if current task is not a real-time task
 */
asmlinkage long sys_exit_np(void)
{
	int retval = -EINVAL;

	TS_EXIT_NP_START;

	if (!is_realtime(current))
		goto out;

	TRACE("sys_exit_np(%s/%d)\n", current->comm, current->pid);
	/* force rescheduling so that we can be preempted */
	set_tsk_need_resched(current);
	retval = 0;
      out:

	TS_EXIT_NP_END;
	return retval;
}

/* p is a real-time task. Re-init its state as a best-effort task. */
static void reinit_litmus_state(struct task_struct* p, int restore)
{
	struct rt_task  user_config = {};
	__user short   *np_flag     = NULL;

	if (restore) {
		/* Safe user-space provided configuration data.
		 * FIXME: This is missing service levels for adaptive tasks.
		 */
		user_config = p->rt_param.task_params;
		np_flag     = p->rt_param.np_flag;
	}

	/* We probably should not be inheriting any task's priority
	 * at this point in time.
	 */
	WARN_ON(p->rt_param.inh_task);

	/* We need to restore the priority of the task. */
//	__setscheduler(p, p->rt_param.old_policy, p->rt_param.old_prio);

	/* Cleanup everything else. */
	memset(&p->rt_param, 0, sizeof(struct rt_task));

	/* Restore preserved fields. */
	if (restore) {
		p->rt_param.task_params = user_config;
		p->rt_param.np_flag      = np_flag;
	}
}

long litmus_admit_task(struct task_struct* tsk)
{
	long retval;
	long flags;

	BUG_ON(is_realtime(tsk));

	if (get_rt_period(tsk) == 0 ||
	    get_exec_cost(tsk) > get_rt_period(tsk)) {
		TRACE_TASK(tsk, "litmus admit: invalid task parameters "
			   "(%lu, %lu)\n",
		       get_exec_cost(tsk), get_rt_period(tsk));
		return -EINVAL;
	}

	if (!cpu_online(get_partition(tsk)))
	{
		TRACE_TASK(tsk, "litmus admit: cpu %d is not online\n",
			   get_partition(tsk));
		return -EINVAL;
	}

	INIT_LIST_HEAD(&tsk->rt_list);

	/* avoid scheduler plugin changing underneath us */
	spin_lock_irqsave(&task_transition_lock, flags);
	retval = litmus->admit_task(tsk);

	if (!retval)
		atomic_inc(&rt_task_count);
	spin_unlock_irqrestore(&task_transition_lock, flags);

	return retval;

}

void litmus_exit_task(struct task_struct* tsk)
{
	if (is_realtime(tsk)) {
		litmus->task_exit(tsk);
		atomic_dec(&rt_task_count);
		reinit_litmus_state(tsk, 1);
	}
}

/* Switching a plugin in use is tricky.
 * We must watch out that no real-time tasks exists
 * (and that none is created in parallel) and that the plugin is not
 * currently in use on any processor (in theory).
 *
 * For now, we don't enforce the second part since it is unlikely to cause
 * any trouble by itself as long as we don't unload modules.
 */
int switch_sched_plugin(struct sched_plugin* plugin)
{
	long flags;
	int ret = 0;

	BUG_ON(!plugin);

	/* stop task transitions */
	spin_lock_irqsave(&task_transition_lock, flags);

	/* don't switch if there are active real-time tasks */
	if (atomic_read(&rt_task_count) == 0) {
		printk(KERN_INFO "Switching to LITMUS^RT plugin %s.\n", plugin->plugin_name);
		litmus = plugin;
	} else
		ret = -EBUSY;

	spin_unlock_irqrestore(&task_transition_lock, flags);
	return ret;
}

/* Called upon fork.
 * p is the newly forked task.
 */
void litmus_fork(struct task_struct* p)
{
	if (is_realtime(p))
		/* clean out any litmus related state, don't preserve anything*/
		reinit_litmus_state(p, 0);
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
		p->rt_param.np_flag = NULL;
	}
}

void exit_litmus(struct task_struct *dead_tsk)
{
	if (is_realtime(dead_tsk))
		litmus_exit_task(dead_tsk);
}


void list_qsort(struct list_head* list, list_cmp_t less_than)
{
	struct list_head lt;
	struct list_head geq;
	struct list_head *pos, *extra, *pivot;
	int n_lt = 0, n_geq = 0;
	BUG_ON(!list);

	if (list->next == list)
		return;

	INIT_LIST_HEAD(&lt);
	INIT_LIST_HEAD(&geq);

	pivot = list->next;
	list_del(pivot);
	list_for_each_safe(pos, extra, list) {
		list_del(pos);
		if (less_than(pos, pivot)) {
			list_add(pos, &lt);
			n_lt++;
		} else {
			list_add(pos, &geq);
			n_geq++;
		}
	}
	if (n_lt < n_geq) {
		list_qsort(&lt, less_than);
		list_qsort(&geq, less_than);
	} else {
		list_qsort(&geq, less_than);
		list_qsort(&lt, less_than);
	}
	list_splice(&geq, list);
	list_add(pivot, list);
	list_splice(&lt, list);
}

#ifdef CONFIG_MAGIC_SYSRQ
int sys_kill(int pid, int sig);

static void sysrq_handle_kill_rt_tasks(int key, struct tty_struct *tty)
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
	.help_msg	= "Quit-rt-tasks",
	.action_msg	= "sent SIGKILL to all real-time tasks",
};
#endif

static int proc_read_stats(char *page, char **start,
			   off_t off, int count,
			   int *eof, void *data)
{
	int len;

	len = snprintf(page, PAGE_SIZE,
		       "real-time task count = %d\n",
		       atomic_read(&rt_task_count));
	return len;
}

static int proc_read_plugins(char *page, char **start,
			   off_t off, int count,
			   int *eof, void *data)
{
	int len;

	len = print_sched_plugins(page, PAGE_SIZE);
	return len;
}

static int proc_read_curr(char *page, char **start,
			  off_t off, int count,
			  int *eof, void *data)
{
	int len;

	len = snprintf(page, PAGE_SIZE, "%s\n", litmus->plugin_name);
	return len;
}

static int proc_write_curr(struct file *file,
			   const char *buffer,
			   unsigned long count,
			   void *data)
{
	int len, ret;
	char name[65];
	struct sched_plugin* found;

	if(count > 64)
		len = 64;
	else
		len = count;

	if(copy_from_user(name, buffer, len))
		return -EFAULT;

	name[len] = '\0';
	/* chomp name */
	if (len > 1 && name[len - 1] == '\n')
		name[len - 1] = '\0';

	found = find_sched_plugin(name);

	if (found) {
		ret = switch_sched_plugin(found);
		if (ret != 0)
			printk(KERN_INFO "Could not switch plugin: %d\n", ret);
	} else
		printk(KERN_INFO "Plugin '%s' is unknown.\n", name);

	return len;
}


static struct proc_dir_entry *litmus_dir = NULL,
	*curr_file = NULL,
	*stat_file = NULL,
	*plugs_file = NULL;

static int __init init_litmus_proc(void)
{
	litmus_dir = proc_mkdir("litmus", NULL);
	if (!litmus_dir) {
		printk(KERN_ERR "Could not allocate LITMUS^RT procfs entry.\n");
		return -ENOMEM;
	}
	litmus_dir->owner = THIS_MODULE;

	curr_file = create_proc_entry("active_plugin",
				      0644, litmus_dir);
	if (!curr_file) {
		printk(KERN_ERR "Could not allocate active_plugin "
		       "procfs entry.\n");
		return -ENOMEM;
	}
	curr_file->owner = THIS_MODULE;
	curr_file->read_proc  = proc_read_curr;
	curr_file->write_proc = proc_write_curr;

	stat_file = create_proc_read_entry("stats", 0444, litmus_dir,
					   proc_read_stats, NULL);

	plugs_file = create_proc_read_entry("plugins", 0444, litmus_dir,
					   proc_read_plugins, NULL);

	return 0;
}

static void exit_litmus_proc(void)
{
	if (plugs_file)
		remove_proc_entry("plugins", litmus_dir);
	if (stat_file)
		remove_proc_entry("stats", litmus_dir);
	if (curr_file)
		remove_proc_entry("active_plugin", litmus_dir);
	if (litmus_dir)
		remove_proc_entry("litmus", NULL);
}

extern struct sched_plugin linux_sched_plugin;

static int __init _init_litmus(void)
{
	/*      Common initializers,
	 *      mode change lock is used to enforce single mode change
	 *      operation.
	 */
	printk("Starting LITMUS^RT kernel\n");

	register_sched_plugin(&linux_sched_plugin);

#ifdef CONFIG_MAGIC_SYSRQ
	/* offer some debugging help */
	if (!register_sysrq_key('q', &sysrq_kill_rt_tasks_op))
		printk("Registered kill rt tasks magic sysrq.\n");
	else
		printk("Could not register kill rt tasks magic sysrq.\n");
#endif

	init_litmus_proc();

	return 0;
}

static void _exit_litmus(void)
{
	exit_litmus_proc();
}

module_init(_init_litmus);
module_exit(_exit_litmus);
