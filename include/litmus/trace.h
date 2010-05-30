#ifndef _SYS_TRACE_H_
#define	_SYS_TRACE_H_

#ifdef CONFIG_SCHED_OVERHEAD_TRACE

#include <litmus/feather_trace.h>
#include <litmus/feather_buffer.h>


/*********************** TIMESTAMPS ************************/

enum task_type_marker {
	TSK_BE,
	TSK_RT,
	TSK_UNKNOWN
};

struct timestamp {
	uint64_t		timestamp;
	uint32_t		seq_no;
	uint8_t			cpu;
	uint8_t			event;
	uint8_t			task_type;
};

/* tracing callbacks */
feather_callback void save_timestamp(unsigned long event);
feather_callback void save_timestamp_def(unsigned long event, unsigned long type);
feather_callback void save_timestamp_task(unsigned long event, unsigned long t_ptr);
feather_callback void save_timestamp_cpu(unsigned long event, unsigned long cpu);


#define TIMESTAMP(id) ft_event0(id, save_timestamp)

#define DTIMESTAMP(id, def)  ft_event1(id, save_timestamp_def, (unsigned long) def)

#define TTIMESTAMP(id, task) \
	ft_event1(id, save_timestamp_task, (unsigned long) task)

#define CTIMESTAMP(id, cpu) \
	ft_event1(id, save_timestamp_cpu, (unsigned long) cpu)

#else /* !CONFIG_SCHED_OVERHEAD_TRACE */

#define TIMESTAMP(id)        /* no tracing */

#define DTIMESTAMP(id, def)  /* no tracing */

#define TTIMESTAMP(id, task) /* no tracing */

#define CTIMESTAMP(id, cpu)  /* no tracing */

#endif


/* Convention for timestamps
 * =========================
 *
 * In order to process the trace files with a common tool, we use the following
 * convention to measure execution times: The end time id of a code segment is
 * always the next number after the start time event id.
 */

#define TS_SCHED_START			DTIMESTAMP(100, TSK_UNKNOWN) /* we only
								      * care
								      * about
								      * next */
#define TS_SCHED_END(t)			TTIMESTAMP(101, t)
#define TS_SCHED2_START(t) 		TTIMESTAMP(102, t)
#define TS_SCHED2_END(t)       		TTIMESTAMP(103, t)

#define TS_CXS_START(t)			TTIMESTAMP(104, t)
#define TS_CXS_END(t)			TTIMESTAMP(105, t)

#define TS_RELEASE_START		DTIMESTAMP(106, TSK_RT)
#define TS_RELEASE_END			DTIMESTAMP(107, TSK_RT)

#define TS_TICK_START(t)		TTIMESTAMP(110, t)
#define TS_TICK_END(t) 			TTIMESTAMP(111, t)


#define TS_PLUGIN_SCHED_START		/* TIMESTAMP(120) */  /* currently unused */
#define TS_PLUGIN_SCHED_END		/* TIMESTAMP(121) */

#define TS_PLUGIN_TICK_START		/* TIMESTAMP(130) */
#define TS_PLUGIN_TICK_END		/* TIMESTAMP(131) */

#define TS_ENTER_NP_START		TIMESTAMP(140)
#define TS_ENTER_NP_END			TIMESTAMP(141)

#define TS_EXIT_NP_START		TIMESTAMP(150)
#define TS_EXIT_NP_END			TIMESTAMP(151)

#define TS_SRP_UP_START			TIMESTAMP(160)
#define TS_SRP_UP_END			TIMESTAMP(161)
#define TS_SRP_DOWN_START		TIMESTAMP(162)
#define TS_SRP_DOWN_END			TIMESTAMP(163)

#define TS_PI_UP_START			TIMESTAMP(170)
#define TS_PI_UP_END			TIMESTAMP(171)
#define TS_PI_DOWN_START		TIMESTAMP(172)
#define TS_PI_DOWN_END			TIMESTAMP(173)

#define TS_FIFO_UP_START		TIMESTAMP(180)
#define TS_FIFO_UP_END			TIMESTAMP(181)
#define TS_FIFO_DOWN_START		TIMESTAMP(182)
#define TS_FIFO_DOWN_END		TIMESTAMP(183)

#define TS_SEND_RESCHED_START(c)	CTIMESTAMP(190, c)
#define TS_SEND_RESCHED_END		DTIMESTAMP(191, TSK_UNKNOWN)


#endif /* !_SYS_TRACE_H_ */
