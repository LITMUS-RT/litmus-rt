/* sched_trace.h -- record scheduler events to a byte stream for offline analysis.
 */
#ifndef _LINUX_SCHED_TRACE_H_
#define _LINUX_SCHED_TRACE_H_

/* all times in nanoseconds */

struct st_trace_header {
	u8	type;		/* Of what type is this record?  */
	u8	cpu;		/* On which CPU was it recorded? */
	u16	pid;		/* PID of the task.              */
	u32	job;		/* The job sequence number.      */
};

struct st_name_data {
	char	cmd[16];	/* The name of the executable of this process. */
};

struct st_param_data {		/* regular params */
	u32	wcet;
	u32	period;
	u32	phase;
	u8	partition;
	u8	__unused[3];
};

struct st_release_data {	/* A job is was/is going to be released. */
	u64	release;	/* What's the release time?              */
	u64	deadline;	/* By when must it finish?		 */
};

struct st_assigned_data {	/* A job was asigned to a CPU. 		 */
	u64	when;
	u8	target;		/* Where should it execute?	         */
	u8	__unused[3];
};

struct st_switch_to_data {	/* A process was switched to on a given CPU.   */
	u64	when;		/* When did this occur?                        */
	u32	exec_time;	/* Time the current job has executed.          */

};

struct st_switch_away_data {	/* A process was switched away from on a given CPU. */
	u64	when;
	u64	exec_time;
};

struct st_completion_data {	/* A job completed. */
	u64	when;
	u8	forced:1; 	/* Set to 1 if job overran and kernel advanced to the
				 * next task automatically; set to 0 otherwise.
				 */
	u8	__uflags:7;
	u8	__unused[3];
};

struct st_block_data {		/* A task blocks. */
	u64	when;
	u64	__unused;
};

struct st_resume_data {		/* A task resumes. */
	u64	when;
	u64	__unused;
};


#define DATA(x) struct st_ ## x ## _data x;

typedef enum {
        ST_NAME = 1,		/* Start at one, so that we can spot
				 * uninitialized records. */
	ST_PARAM,
	ST_RELEASE,
	ST_ASSIGNED,
	ST_SWITCH_TO,
	ST_SWITCH_AWAY,
	ST_COMPLETION,
	ST_BLOCK,
	ST_RESUME
} st_event_record_type_t;

struct st_event_record {
	struct st_trace_header hdr;
	union {
		u64 raw[2];

		DATA(name);
		DATA(param);
		DATA(release);
		DATA(assigned);
		DATA(switch_to);
		DATA(switch_away);
		DATA(completion);
		DATA(block);
		DATA(resume);

	} data;
};

#undef DATA

#ifdef __KERNEL__

#include <linux/sched.h>

/* dummies, need to be re-implemented */
/* used in sched.c */
#define sched_trace_task_arrival(t)
#define sched_trace_task_departure(t)
#define sched_trace_task_preemption(t, by)
#define sched_trace_task_scheduled(t)

/* used in scheduler plugins */
#define sched_trace_job_release(t)
#define sched_trace_job_completion(t)



#ifdef CONFIG_SCHED_TASK_TRACE




#else


#endif


#ifdef CONFIG_SCHED_DEBUG_TRACE
void sched_trace_log_message(const char* fmt, ...);

#else

#define sched_trace_log_message(fmt, ...)

#endif

#endif /* __KERNEL__ */

#endif
