#ifndef LITMUS_DEBUG_TRACE_H
#define LITMUS_DEBUG_TRACE_H

#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);
void dump_trace_buffer(int max);
#else

#define sched_trace_log_message(fmt, ...)

#endif

extern atomic_t __log_seq_no;

#define TRACE(fmt, args...) \
	sched_trace_log_message("%d P%d: " fmt, atomic_add_return(1, &__log_seq_no), \
				raw_smp_processor_id(), ## args)

#define TRACE_TASK(t, fmt, args...) \
	TRACE("(%s/%d) " fmt, (t)->comm, (t)->pid, ##args)

#define TRACE_CUR(fmt, args...) \
	TRACE_TASK(current, fmt, ## args)

#define TRACE_BUG_ON(cond) \
	do { if (cond) TRACE("BUG_ON(%s) at %s:%d " \
			     "called from %p current=%s/%d state=%d " \
			     "flags=%x partition=%d cpu=%d rtflags=%d"\
			     " job=%u timeslice=%u\n",		\
	#cond, __FILE__, __LINE__, __builtin_return_address(0), current->comm, \
	current->pid, current->state, current->flags,  \
	get_partition(current), smp_processor_id(), get_rt_flags(current), \
	current->rt_param.job_params.job_no, \
	current->rt.time_slice\
	); } while(0);

#endif
