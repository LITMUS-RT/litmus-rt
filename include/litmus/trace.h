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
	uint64_t		timestamp:48;
	uint64_t		pid:16;
	uint32_t		seq_no;
	uint8_t			cpu;
	uint8_t			event;
	uint8_t			task_type:2;
	uint8_t			irq_flag:1;
	uint8_t			irq_count:5;
};

/* tracing callbacks */
feather_callback void msg_sent(unsigned long event, unsigned long to);
feather_callback void msg_received(unsigned long event);

#define MSG_TIMESTAMP_SENT(id, to) \
	ft_event1(id, msg_sent, (unsigned long) to);

#define MSG_TIMESTAMP_RECEIVED(id) \
	ft_event0(id, msg_received);

feather_callback void save_cpu_timestamp(unsigned long event);
feather_callback void save_cpu_timestamp_time(unsigned long event, unsigned long time_ptr);
feather_callback void save_cpu_timestamp_irq(unsigned long event, unsigned long irq_count_ptr);
feather_callback void save_cpu_timestamp_task(unsigned long event, unsigned long t_ptr);
feather_callback void save_cpu_timestamp_def(unsigned long event, unsigned long type);
feather_callback void save_cpu_task_latency(unsigned long event, unsigned long when_ptr);

#define CPU_TIMESTAMP_TIME(id, time_ptr) \
	ft_event1(id, save_cpu_timestamp_time, (unsigned long) time_ptr)

#define CPU_TIMESTAMP_IRQ(id, irq_count_ptr) \
	ft_event1(id, save_cpu_timestamp_irq, (unsigned long) irq_count_ptr)

#define CPU_TIMESTAMP(id) ft_event0(id, save_cpu_timestamp)

#define CPU_DTIMESTAMP(id, def)  ft_event1(id, save_cpu_timestamp_def, (unsigned long) def)

#define CPU_TIMESTAMP_CUR(id) CPU_DTIMESTAMP(id, is_realtime(current) ? TSK_RT : TSK_BE)

#define CPU_TTIMESTAMP(id, task) \
	ft_event1(id, save_cpu_timestamp_task, (unsigned long) task)

#define CPU_LTIMESTAMP(id, task) \
	ft_event1(id, save_cpu_task_latency, (unsigned long) task)

#else /* !CONFIG_SCHED_OVERHEAD_TRACE */

#define MSG_TIMESTAMP_SENT(id, to)
#define MSG_TIMESTAMP_RECEIVED(id)

#define CPU_TIMESTAMP_TIME(id, time_ptr)
#define CPU_TIMESTAMP_IRQ(id, irq_count_ptr)
#define CPU_TIMESTAMP(id)
#define CPU_DTIMESTAMP(id, def)
#define CPU_TIMESTAMP_CUR(id)
#define CPU_TTIMESTAMP(id, task)
#define CPU_LTIMESTAMP(id, task)

#endif


/* Convention for timestamps
 * =========================
 *
 * In order to process the trace files with a common tool, we use the following
 * convention to measure execution times: The end time id of a code segment is
 * always the next number after the start time event id.
 */

#define __TS_SYSCALL_IN_START(p)	CPU_TIMESTAMP_TIME(10, p)
#define __TS_SYSCALL_IN_END(p)		CPU_TIMESTAMP_IRQ(11, p)

#define TS_SYSCALL_OUT_START		CPU_TIMESTAMP_CUR(20)
#define TS_SYSCALL_OUT_END		CPU_TIMESTAMP_CUR(21)

#define TS_LOCK_START			CPU_TIMESTAMP_CUR(30)
#define TS_LOCK_END			CPU_TIMESTAMP_CUR(31)

#define TS_LOCK_SUSPEND			CPU_TIMESTAMP_CUR(38)
#define TS_LOCK_RESUME			CPU_TIMESTAMP_CUR(39)

#define TS_UNLOCK_START			CPU_TIMESTAMP_CUR(40)
#define TS_UNLOCK_END			CPU_TIMESTAMP_CUR(41)

#define TS_SCHED_START			CPU_DTIMESTAMP(100, TSK_UNKNOWN) /* we only
								      * care
								      * about
								      * next */
#define TS_SCHED_END(t)			CPU_TTIMESTAMP(101, t)
#define TS_SCHED2_START(t) 		CPU_TTIMESTAMP(102, t)
#define TS_SCHED2_END(t)       		CPU_TTIMESTAMP(103, t)

#define TS_CXS_START(t)			CPU_TTIMESTAMP(104, t)
#define TS_CXS_END(t)			CPU_TTIMESTAMP(105, t)

#define TS_RELEASE_START		CPU_DTIMESTAMP(106, TSK_RT)
#define TS_RELEASE_END			CPU_DTIMESTAMP(107, TSK_RT)

#define TS_TICK_START(t)		CPU_TTIMESTAMP(110, t)
#define TS_TICK_END(t) 			CPU_TTIMESTAMP(111, t)

#define TS_QUANTUM_BOUNDARY_START	CPU_TIMESTAMP_CUR(112)
#define TS_QUANTUM_BOUNDARY_END		CPU_TIMESTAMP_CUR(113)


#define TS_PLUGIN_SCHED_START		/* TIMESTAMP(120) */  /* currently unused */
#define TS_PLUGIN_SCHED_END		/* TIMESTAMP(121) */

#define TS_PLUGIN_TICK_START		/* TIMESTAMP(130) */
#define TS_PLUGIN_TICK_END		/* TIMESTAMP(131) */

#define TS_ENTER_NP_START		CPU_TIMESTAMP(140)
#define TS_ENTER_NP_END			CPU_TIMESTAMP(141)

#define TS_EXIT_NP_START		CPU_TIMESTAMP(150)
#define TS_EXIT_NP_END			CPU_TIMESTAMP(151)

#define TS_SEND_RESCHED_START(c)	MSG_TIMESTAMP_SENT(190, c)
#define TS_SEND_RESCHED_END		MSG_TIMESTAMP_RECEIVED(191)

#define TS_RELEASE_LATENCY(when)	CPU_LTIMESTAMP(208, &(when))

#endif /* !_SYS_TRACE_H_ */
