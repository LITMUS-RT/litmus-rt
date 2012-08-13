

#include <litmus/sched_plugin.h>
#include <litmus/preempt.h>
#include <litmus/debug_trace.h>

static struct task_struct* demo_schedule(struct task_struct * prev)
{
	/* This mandatory. It triggers a transition in the LITMUS^RT remote
	 * preemption state machine. Call this AFTER the plugin has made a local
	 * scheduling decision.
	 */
	sched_state_task_picked();

	/* We don't schedule anything for now. NULL means "schedule background work". */
	return NULL;
}

static long demo_admit_task(struct task_struct *tsk)
{
	TRACE_TASK(tsk, "rejected by demo plugin.\n");

	/* Reject every task. */
	return -EINVAL;
}

static struct sched_plugin demo_plugin = {
	.plugin_name		= "DEMO",
	.schedule		= demo_schedule,
	.admit_task		= demo_admit_task,
};

static int __init init_demo(void)
{
	return register_sched_plugin(&demo_plugin);
}

module_init(init_demo);

