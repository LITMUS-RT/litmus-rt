#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/preempt.h>
#include <litmus/trace.h>

/* The rescheduling state of each processor.
 */
DEFINE_PER_CPU_SHARED_ALIGNED(atomic_t, resched_state);

void sched_state_will_schedule(struct task_struct* tsk)
{
	/* Litmus hack: we only care about processor-local invocations of
	 * set_tsk_need_resched(). We can't reliably set the flag remotely
	 * since it might race with other updates to the scheduling state.  We
	 * can't rely on the runqueue lock protecting updates to the sched
	 * state since processors do not acquire the runqueue locks for all
	 * updates to the sched state (to avoid acquiring two runqueue locks at
	 * the same time). Further, if tsk is residing on a remote processor,
	 * then that processor doesn't actually know yet that it is going to
	 * reschedule; it still must receive an IPI (unless a local invocation
	 * races).
	 */
	if (likely(task_cpu(tsk) == smp_processor_id())) {
		VERIFY_SCHED_STATE(TASK_SCHEDULED | SHOULD_SCHEDULE | TASK_PICKED | WILL_SCHEDULE);
		if (is_in_sched_state(TASK_PICKED | PICKED_WRONG_TASK))
			set_sched_state(PICKED_WRONG_TASK);
		else
			set_sched_state(WILL_SCHEDULE);
	} else
		/* Litmus tasks should never be subject to a remote
		 * set_tsk_need_resched(). */
		BUG_ON(is_realtime(tsk));
#ifdef CONFIG_PREEMPT_STATE_TRACE
	TRACE_TASK(tsk, "set_tsk_need_resched() ret:%p\n",
		   __builtin_return_address(0));
#endif
}

/* Called by the IPI handler after another CPU called smp_send_resched(). */
void sched_state_ipi(void)
{
	/* If the IPI was slow, we might be in any state right now. The IPI is
	 * only meaningful if we are in SHOULD_SCHEDULE. */
	if (is_in_sched_state(SHOULD_SCHEDULE)) {
		/* Cause scheduler to be invoked.
		 * This will cause a transition to WILL_SCHEDULE. */
		set_tsk_need_resched(current);
		TRACE_STATE("IPI -> set_tsk_need_resched(%s/%d)\n",
			    current->comm, current->pid);
		TS_SEND_RESCHED_END;
	} else {
		/* ignore */
		TRACE_STATE("ignoring IPI in state %x (%s)\n",
			    get_sched_state(),
			    sched_state_name(get_sched_state()));
	}
}

/* Called by plugins to cause a CPU to reschedule. IMPORTANT: the caller must
 * hold the lock that is used to serialize scheduling decisions. */
void litmus_reschedule(int cpu)
{
	int picked_transition_ok = 0;
	int scheduled_transition_ok = 0;

	/* The (remote) CPU could be in any state. */

	/* The critical states are TASK_PICKED and TASK_SCHEDULED, as the CPU
	 * is not aware of the need to reschedule at this point. */

	/* is a context switch in progress? */
	if (cpu_is_in_sched_state(cpu, TASK_PICKED))
		picked_transition_ok = sched_state_transition_on(
			cpu, TASK_PICKED, PICKED_WRONG_TASK);

	if (!picked_transition_ok &&
	    cpu_is_in_sched_state(cpu, TASK_SCHEDULED)) {
		/* We either raced with the end of the context switch, or the
		 * CPU was in TASK_SCHEDULED anyway. */
		scheduled_transition_ok = sched_state_transition_on(
			cpu, TASK_SCHEDULED, SHOULD_SCHEDULE);
	}

	/* If the CPU was in state TASK_SCHEDULED, then we need to cause the
	 * scheduler to be invoked. */
	if (scheduled_transition_ok) {
		if (smp_processor_id() == cpu)
			set_tsk_need_resched(current);
		else {
			TS_SEND_RESCHED_START(cpu);
			smp_send_reschedule(cpu);
		}
	}

	TRACE_STATE("%s picked-ok:%d sched-ok:%d\n",
		    __FUNCTION__,
		    picked_transition_ok,
		    scheduled_transition_ok);
}

void litmus_reschedule_local(void)
{
	if (is_in_sched_state(TASK_PICKED))
		set_sched_state(PICKED_WRONG_TASK);
	else if (is_in_sched_state(TASK_SCHEDULED | SHOULD_SCHEDULE)) {
		set_sched_state(WILL_SCHEDULE);
		set_tsk_need_resched(current);
	}
}

#ifdef CONFIG_DEBUG_KERNEL

void sched_state_plugin_check(void)
{
	if (!is_in_sched_state(TASK_PICKED | PICKED_WRONG_TASK)) {
		TRACE("!!!! plugin did not call sched_state_task_picked()!"
		      "Calling sched_state_task_picked() is mandatory---fix this.\n");
		set_sched_state(TASK_PICKED);
	}
}

#define NAME_CHECK(x) case x:  return #x
const char* sched_state_name(int s)
{
	switch (s) {
		NAME_CHECK(TASK_SCHEDULED);
		NAME_CHECK(SHOULD_SCHEDULE);
		NAME_CHECK(WILL_SCHEDULE);
		NAME_CHECK(TASK_PICKED);
		NAME_CHECK(PICKED_WRONG_TASK);
	default:
		return "UNKNOWN";
	};
}

#endif
