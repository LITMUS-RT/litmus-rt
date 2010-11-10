/* sched_plugin.c -- core infrastructure for the scheduler plugin system
 *
 * This file includes the initialization of the plugin system, the no-op Linux
 * scheduler plugin, some dummy functions, and some helper functions.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/preempt.h>
#include <litmus/jobs.h>

/*
 * Generic function to trigger preemption on either local or remote cpu
 * from scheduler plugins. The key feature is that this function is
 * non-preemptive section aware and does not invoke the scheduler / send
 * IPIs if the to-be-preempted task is actually non-preemptive.
 */
void preempt_if_preemptable(struct task_struct* t, int cpu)
{
	/* t is the real-time task executing on CPU on_cpu If t is NULL, then
	 * on_cpu is currently scheduling background work.
	 */

	int reschedule = 0;

	if (!t)
		/* move non-real-time task out of the way */
		reschedule = 1;
	else {
		if (smp_processor_id() == cpu) {
			/* local CPU case */
			/* check if we need to poke userspace */
			if (is_user_np(t))
				/* yes, poke it */
				request_exit_np(t);
			else if (!is_kernel_np(t))
				/* only if we are allowed to preempt the
				 * currently-executing task */
				reschedule = 1;
		} else {
			/* remote CPU case */
			if (is_user_np(t)) {
				/* need to notify user space of delayed
				 * preemption */

				/* to avoid a race, set the flag, then test
				 * again */
				request_exit_np(t);
				/* make sure it got written */
				mb();
			}
			/* Only send an ipi if remote task might have raced our
			 * request, i.e., send an IPI to make sure in case it
			 * exited its critical section.
			 */
			reschedule = !is_np(t) && !is_kernel_np(t);
		}
	}
	if (likely(reschedule))
		litmus_reschedule(cpu);
}


/*************************************************************
 *                   Dummy plugin functions                  *
 *************************************************************/

static void litmus_dummy_finish_switch(struct task_struct * prev)
{
}

static struct task_struct* litmus_dummy_schedule(struct task_struct * prev)
{
	sched_state_task_picked();
	return NULL;
}

static void litmus_dummy_tick(struct task_struct* tsk)
{
}

static long litmus_dummy_admit_task(struct task_struct* tsk)
{
	printk(KERN_CRIT "LITMUS^RT: Linux plugin rejects %s/%d.\n",
		tsk->comm, tsk->pid);
	return -EINVAL;
}

static void litmus_dummy_task_new(struct task_struct *t, int on_rq, int running)
{
}

static void litmus_dummy_task_wake_up(struct task_struct *task)
{
}

static void litmus_dummy_task_block(struct task_struct *task)
{
}

static void litmus_dummy_task_exit(struct task_struct *task)
{
}

static long litmus_dummy_complete_job(void)
{
	return -ENOSYS;
}

static long litmus_dummy_activate_plugin(void)
{
	return 0;
}

static long litmus_dummy_deactivate_plugin(void)
{
	return 0;
}

#ifdef CONFIG_FMLP

static long litmus_dummy_inherit_priority(struct pi_semaphore *sem,
					  struct task_struct *new_owner)
{
	return -ENOSYS;
}

static long litmus_dummy_return_priority(struct pi_semaphore *sem)
{
	return -ENOSYS;
}

static long litmus_dummy_pi_block(struct pi_semaphore *sem,
				  struct task_struct *new_waiter)
{
	return -ENOSYS;
}

#endif


/* The default scheduler plugin. It doesn't do anything and lets Linux do its
 * job.
 */
struct sched_plugin linux_sched_plugin = {
	.plugin_name = "Linux",
	.tick = litmus_dummy_tick,
	.task_new   = litmus_dummy_task_new,
	.task_exit = litmus_dummy_task_exit,
	.task_wake_up = litmus_dummy_task_wake_up,
	.task_block = litmus_dummy_task_block,
	.complete_job = litmus_dummy_complete_job,
	.schedule = litmus_dummy_schedule,
	.finish_switch = litmus_dummy_finish_switch,
	.activate_plugin = litmus_dummy_activate_plugin,
	.deactivate_plugin = litmus_dummy_deactivate_plugin,
#ifdef CONFIG_FMLP
	.inherit_priority = litmus_dummy_inherit_priority,
	.return_priority = litmus_dummy_return_priority,
	.pi_block = litmus_dummy_pi_block,
#endif
	.admit_task = litmus_dummy_admit_task
};

/*
 *	The reference to current plugin that is used to schedule tasks within
 *	the system. It stores references to actual function implementations
 *	Should be initialized by calling "init_***_plugin()"
 */
struct sched_plugin *litmus = &linux_sched_plugin;

/* the list of registered scheduling plugins */
static LIST_HEAD(sched_plugins);
static DEFINE_RAW_SPINLOCK(sched_plugins_lock);

#define CHECK(func) {\
	if (!plugin->func) \
		plugin->func = litmus_dummy_ ## func;}

/* FIXME: get reference to module  */
int register_sched_plugin(struct sched_plugin* plugin)
{
	printk(KERN_INFO "Registering LITMUS^RT plugin %s.\n",
	       plugin->plugin_name);

	/* make sure we don't trip over null pointers later */
	CHECK(finish_switch);
	CHECK(schedule);
	CHECK(tick);
	CHECK(task_wake_up);
	CHECK(task_exit);
	CHECK(task_block);
	CHECK(task_new);
	CHECK(complete_job);
	CHECK(activate_plugin);
	CHECK(deactivate_plugin);
#ifdef CONFIG_FMLP
	CHECK(inherit_priority);
	CHECK(return_priority);
	CHECK(pi_block);
#endif
	CHECK(admit_task);

	if (!plugin->release_at)
		plugin->release_at = release_at;

	raw_spin_lock(&sched_plugins_lock);
	list_add(&plugin->list, &sched_plugins);
	raw_spin_unlock(&sched_plugins_lock);

	return 0;
}


/* FIXME: reference counting, etc. */
struct sched_plugin* find_sched_plugin(const char* name)
{
	struct list_head *pos;
	struct sched_plugin *plugin;

	raw_spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		if (!strcmp(plugin->plugin_name, name))
		    goto out_unlock;
	}
	plugin = NULL;

out_unlock:
	raw_spin_unlock(&sched_plugins_lock);
	return plugin;
}

int print_sched_plugins(char* buf, int max)
{
	int count = 0;
	struct list_head *pos;
	struct sched_plugin *plugin;

	raw_spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		count += snprintf(buf + count, max - count, "%s\n", plugin->plugin_name);
		if (max - count <= 0)
			break;
	}
	raw_spin_unlock(&sched_plugins_lock);
	return 	count;
}
