
#ifndef _SYS_TRACE_H_
#define	_SYS_TRACE_H_

#include <litmus/feather_trace.h>
#include <litmus/feather_buffer.h>


/*********************** TIMESTAMPS ************************/

struct timestamp {
	unsigned long		event;
	unsigned long long	timestamp;
	unsigned int		seq_no;
	int			cpu;
};


/* buffer holding time stamps - will be provided by driver */
extern struct ft_buffer* trace_ts_buf;

/* save_timestamp:  stores current time as struct timestamp
 * in trace_ts_buf
 */
asmlinkage void save_timestamp(unsigned long event);

#define TIMESTAMP(id) ft_event0(id, save_timestamp)

/* Convention for timestamps
 * =========================
 *
 * In order to process the trace files with a common tool, we use the following
 * convention to measure execution times: The end time id of a code segment is
 * always the next number after the start time event id.
 */

#define TS_SCHED_START 			TIMESTAMP(100)
#define TS_SCHED_END			TIMESTAMP(101)
#define TS_CXS_START			TIMESTAMP(102)
#define TS_CXS_END			TIMESTAMP(103)

#define TS_TICK_START  			TIMESTAMP(110)
#define TS_TICK_END    			TIMESTAMP(111)

#define TS_PLUGIN_SCHED_START		TIMESTAMP(120)
#define TS_PLUGIN_SCHED_END		TIMESTAMP(121)

#define TS_PLUGIN_TICK_START		TIMESTAMP(130)
#define TS_PLUGIN_TICK_END		TIMESTAMP(131)

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



#endif /* !_SYS_TRACE_H_ */
