/* sched_plugin.c -- core infrastructure for the scheduler plugin system
 *
 * This file includes the initialization of the plugin system, the no-op Linux
 * scheduler plugin and some dummy functions.
 */

#include <linux/list.h>
#include <linux/spinlock.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>


/*************************************************************
 *                   Dummy plugin functions                  *
 *************************************************************/

static void litmus_dummy_finish_switch(struct task_struct * prev)
{
}

static struct task_struct* litmus_dummy_schedule(struct task_struct * prev)
{
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
	.inherit_priority = litmus_dummy_inherit_priority,
	.return_priority = litmus_dummy_return_priority,
	.pi_block = litmus_dummy_pi_block,
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
static DEFINE_SPINLOCK(sched_plugins_lock);

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
	CHECK(inherit_priority);
	CHECK(return_priority);
	CHECK(pi_block);
	CHECK(admit_task);

	spin_lock(&sched_plugins_lock);
	list_add(&plugin->list, &sched_plugins);
	spin_unlock(&sched_plugins_lock);

	return 0;
}


/* FIXME: reference counting, etc. */
struct sched_plugin* find_sched_plugin(const char* name)
{
	struct list_head *pos;
	struct sched_plugin *plugin;

	spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		if (!strcmp(plugin->plugin_name, name))
		    goto out_unlock;
	}
	plugin = NULL;

out_unlock:
	spin_unlock(&sched_plugins_lock);
	return plugin;
}

int print_sched_plugins(char* buf, int max)
{
	int count = 0;
	struct list_head *pos;
	struct sched_plugin *plugin;

	spin_lock(&sched_plugins_lock);
	list_for_each(pos, &sched_plugins) {
		plugin = list_entry(pos, struct sched_plugin, list);
		count += snprintf(buf + count, max - count, "%s\n", plugin->plugin_name);
		if (max - count <= 0)
			break;
	}
	spin_unlock(&sched_plugins_lock);
	return 	count;
}
