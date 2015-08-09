#ifndef LITMUS_DEBUG_TRACE_H
#define LITMUS_DEBUG_TRACE_H

#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);
void dump_trace_buffer(int max);
#else

#define sched_trace_log_message(fmt, ...)

#endif

extern atomic_t __log_seq_no;

#ifdef CONFIG_SCHED_DEBUG_TRACE_CALLER
#define TRACE_PREFIX "%d P%d [%s@%s:%d]: "
#define TRACE_ARGS  atomic_add_return(1, &__log_seq_no),	\
		raw_smp_processor_id(),				\
		__FUNCTION__, __FILE__, __LINE__
#else
#define TRACE_PREFIX "%d P%d: "
#define TRACE_ARGS  atomic_add_return(1, &__log_seq_no), \
		raw_smp_processor_id()
#endif

#define TRACE(fmt, args...)						\
	sched_trace_log_message(TRACE_PREFIX fmt,			\
				TRACE_ARGS,  ## args)

#define TRACE_TASK(t, fmt, args...)			\
	TRACE("(%s/%d:%d) " fmt,			 \
	      t ? (t)->comm : "null",			 \
	      t ? (t)->pid : 0,				 \
	      t ? (t)->rt_param.job_params.job_no : 0,	 \
	      ##args)

#define TRACE_CUR(fmt, args...) \
	TRACE_TASK(current, fmt, ## args)

#endif
