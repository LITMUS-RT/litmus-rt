#include <linux/percpu.h>

#include <litmus/sched_plugin.h>
#include <litmus/preempt.h>
#include <litmus/debug_trace.h>

#include <litmus/litmus.h>
#include <litmus/rt_domain.h>
#include <litmus/edf_common.h>
#include <litmus/jobs.h>
#include <litmus/budget.h>

struct demo_cpu_state {
	rt_domain_t	local_queues;
	int		cpu;

	struct task_struct* scheduled;
};

static DEFINE_PER_CPU(struct demo_cpu_state, demo_cpu_state);

#define cpu_state_for(cpu_id)	(&per_cpu(demo_cpu_state, cpu_id))
#define local_cpu_state()	(&__get_cpu_var(demo_cpu_state))


/* this helper is called when task `prev` exhausted its budget of when
 * it signaled a job completion */
static void demo_job_completion(struct task_struct *prev, int buget_exhausted)
{
	/* call common helper code to compute the next release time, deadline,
	 * etc. */
	prepare_for_next_period(prev);
}

/* Add the task `tsk` to the appropriate queue. Assumes caller holds the ready lock.
 */
static void demo_requeue(struct task_struct *tsk, struct demo_cpu_state *cpu_state)
{
	if (is_released(tsk, litmus_clock())) {
		/* Uses __add_ready() instead of add_ready() because we already
		 * hold the ready lock. */
		__add_ready(&cpu_state->local_queues, tsk);
	} else {
		/* Uses add_release() because we DON'T have the release lock. */
		add_release(&cpu_state->local_queues, tsk);
	}
}

static struct task_struct* demo_schedule(struct task_struct * prev)
{
	struct demo_cpu_state *local_state = local_cpu_state();

	/* next == NULL means "schedule background work". */
	struct task_struct *next = NULL;

	/* prev's task state */
	int exists, out_of_time, job_completed, self_suspends, preempt, resched;

	raw_spin_lock(&local_state->local_queues.ready_lock);

	BUG_ON(local_state->scheduled && local_state->scheduled != prev);
	BUG_ON(local_state->scheduled && !is_realtime(prev));

	exists = local_state->scheduled != NULL;
	self_suspends = exists && !is_running(prev);
	out_of_time   = exists && budget_enforced(prev)
		&& budget_exhausted(prev);
	job_completed = exists && is_completed(prev);

	/* preempt is true if task `prev` has lower priority than something on
	 * the ready queue. */
	preempt = edf_preemption_needed(&local_state->local_queues, prev);

	/* check all conditions that make us reschedule */
	resched = preempt;

	/* if `prev` suspends, it CANNOT be scheduled anymore => reschedule */
	if (self_suspends)
		resched = 1;

	/* also check for (in-)voluntary job completions */
	if (out_of_time || job_completed) {
		demo_job_completion(prev, out_of_time);
		resched = 1;
	}

	if (resched) {
		/* First check if the previous task goes back onto the ready
		 * queue, which it does if it did not self_suspend.
		 */
		if (exists && !self_suspends)
			demo_requeue(prev, local_state);
		next = __take_ready(&local_state->local_queues);
	} else
		/* No preemption is required. */
		next = local_state->scheduled;

	local_state->scheduled = next;


	if (exists && prev != next)
		TRACE_TASK(prev, "descheduled.\n");
	if (next)
		TRACE_TASK(next, "scheduled.\n");

	/* This mandatory. It triggers a transition in the LITMUS^RT remote
	 * preemption state machine. Call this AFTER the plugin has made a local
	 * scheduling decision.
	 */
	sched_state_task_picked();

	raw_spin_unlock(&local_state->local_queues.ready_lock);

	return next;
}

static long demo_admit_task(struct task_struct *tsk)
{
	TRACE_TASK(tsk, "rejected by demo plugin.\n");

	/* Reject every task. */
	return -EINVAL;
}

static long demo_activate_plugin(void)
{
	int cpu;
	struct demo_cpu_state *state;

	for_each_online_cpu(cpu) {
		TRACE("Initializing CPU%d...\n", cpu);

		state = cpu_state_for(cpu);

		state->cpu = cpu;
		state->scheduled = NULL;
		edf_domain_init(&state->local_queues, NULL, NULL);
	}

	return 0;
}

static struct sched_plugin demo_plugin = {
	.plugin_name		= "DEMO",
	.schedule		= demo_schedule,
	.admit_task		= demo_admit_task,
	.activate_plugin	= demo_activate_plugin,
};

static int __init init_demo(void)
{
	return register_sched_plugin(&demo_plugin);
}

module_init(init_demo);

