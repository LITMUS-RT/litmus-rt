/* sched_trace.h -- record scheduler events to a byte stream for offline analysis.
 */
#ifndef _LINUX_SCHED_TRACE_H_
#define _LINUX_SCHED_TRACE_H_

#include <linux/sched.h>

/* dummies, need to be re-implemented */

/* used in sched.c */
#define  sched_trace_task_arrival(t)
#define sched_trace_task_departure(t)
#define sched_trace_task_preemption(t, by)
#define sched_trace_task_scheduled(t)

/* used in scheduler plugins */
#define sched_trace_job_release(t)
#define sched_trace_job_completion(t)


#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);

#else

#define sched_trace_log_message(fmt, ...)

#endif


#endif
