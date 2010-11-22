#ifndef LITMUS_DEBUG_TRACE_H
#define LITMUS_DEBUG_TRACE_H

#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);
void dump_trace_buffer(int max);
#else

#define sched_trace_log_message(fmt, ...)

#endif

extern atomic_t __log_seq_no;

#define TRACE(fmt, args...)						\
	sched_trace_log_message("%d P%d: " fmt,				\
				atomic_add_return(1, &__log_seq_no),	\
				raw_smp_processor_id(), ## args)

#define TRACE_TASK(t, fmt, args...)			\
	TRACE("(%s/%d:%d) " fmt, (t)->comm, (t)->pid,	\
	      (t)->rt_param.job_params.job_no,  ##args)

#define TRACE_CUR(fmt, args...) \
	TRACE_TASK(current, fmt, ## args)

#endif
