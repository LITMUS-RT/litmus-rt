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
#define LITMUS_TRACE_PREFIX "%d P%d [%s@%s:%d]: "
#define LITMUS_TRACE_ARGS  atomic_add_return(1, &__log_seq_no),	\
		raw_smp_processor_id(),				\
		__FUNCTION__, __FILE__, __LINE__
#else
#define LITMUS_TRACE_PREFIX "%d P%d: "
#define LITMUS_TRACE_ARGS  atomic_add_return(1, &__log_seq_no), \
		raw_smp_processor_id()
#endif

#define LITMUS_TRACE(fmt, args...)						\
	sched_trace_log_message(LITMUS_TRACE_PREFIX fmt,			\
				LITMUS_TRACE_ARGS,  ## args)

#define LITMUS_TRACE_TASK(t, fmt, args...)			\
	LITMUS_TRACE("(%s/%d:%d) " fmt,			 \
	      t ? (t)->comm : "null",			 \
	      t ? (t)->pid : 0,				 \
	      t ? (t)->rt_param.job_params.job_no : 0,	 \
	      ##args)

#define LITMUS_TRACE_CUR(fmt, args...) \
	LITMUS_TRACE_TASK(current, fmt, ## args)

#define LITMUS_TRACE_WARN_ON(cond) \
	if (unlikely(cond)) \
		LITMUS_TRACE("WARNING: '%s' [%s@%s:%d]\n", \
			#cond, __FUNCTION__, __FILE__, __LINE__)

#endif

#ifndef LITMUS_DEBUG_TRACE_DONT_POLLUTE_NAMESPACE
#ifndef LITMUS_DEBUG_TRACE_H_UNQUALIFIED_NAMES

#define LITMUS_DEBUG_TRACE_H_UNQUALIFIED_NAMES
#define TRACE(fmt, args...) LITMUS_TRACE(fmt, ## args)
#define TRACE_TASK(t, fmt, args...) LITMUS_TRACE_TASK(t, fmt, ## args)
#define TRACE_CUR(fmt, args...) LITMUS_TRACE_CUR(fmt, ## args)
#define TRACE_WARN_ON(cond) LITMUS_TRACE_WARN_ON(cond)

#endif
#endif
