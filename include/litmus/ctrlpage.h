#ifndef _LITMUS_CTRLPAGE_H_
#define _LITMUS_CTRLPAGE_H_

#include <litmus/rt_param.h>

union np_flag {
	uint32_t raw;
	struct {
		/* Is the task currently in a non-preemptive section? */
		uint32_t flag:31;
		/* Should the task call into the scheduler? */
		uint32_t preempt:1;
	} np;
};

/* The definition of the data that is shared between the kernel and real-time
 * tasks via a shared page (see litmus/ctrldev.c).
 *
 * WARNING: User space can write to this, so don't trust
 * the correctness of the fields!
 *
 * This servees two purposes: to enable efficient signaling
 * of non-preemptive sections (user->kernel) and
 * delayed preemptions (kernel->user), and to export
 * some real-time relevant statistics such as preemption and
 * migration data to user space. We can't use a device to export
 * statistics because we want to avoid system call overhead when
 * determining preemption/migration overheads).
 */
struct control_page {
	/* This flag is used by userspace to communicate non-preempive
	 * sections. */
	volatile __attribute__ ((aligned (8))) union np_flag sched;

	/* Incremented by the kernel each time an IRQ is handled. */
	volatile __attribute__ ((aligned (8))) uint64_t irq_count;

	/* Locking overhead tracing: userspace records here the time stamp
	 * and IRQ counter prior to starting the system call. */
	uint64_t ts_syscall_start;  /* Feather-Trace cycles */
	uint64_t irq_syscall_start; /* Snapshot of irq_count when the syscall
				     * started. */

	lt_t deadline; /* Deadline for the currently executing job */
	lt_t release;  /* Release time of current job */
	uint64_t job_index; /* Job sequence number of current job */

	/* to be extended */
};

/* Expected offsets within the control page. */

#define LITMUS_CP_OFFSET_SCHED		0
#define LITMUS_CP_OFFSET_IRQ_COUNT	8
#define LITMUS_CP_OFFSET_TS_SC_START	16
#define LITMUS_CP_OFFSET_IRQ_SC_START	24
#define LITMUS_CP_OFFSET_DEADLINE	32
#define LITMUS_CP_OFFSET_RELEASE	40
#define LITMUS_CP_OFFSET_JOB_INDEX	48

/* System call emulation via ioctl() */

typedef enum {
	LRT_null_call = 2006,
	LRT_set_rt_task_param,
	LRT_get_rt_task_param,
	LRT_reservation_create,
	LRT_complete_job,
	LRT_od_open,
	LRT_od_close,
	LRT_litmus_lock,
	LRT_litmus_unlock,
	LRT_wait_for_job_release,
	LRT_wait_for_ts_release,
	LRT_release_ts,
	LRT_get_current_budget,
} litmus_syscall_id_t;

union litmus_syscall_args {
	struct {
		pid_t pid;
		struct rt_task __user *param;
	} get_set_task_param;

	struct {
		uint32_t type;
		void __user *config;
	} reservation_create;

	struct {
		uint32_t fd;
		uint32_t obj_type;
		uint32_t obj_id;
		void __user *config;
	} od_open;

	struct {
		lt_t __user *expended;
		lt_t __user *remaining;
	} get_current_budget;
};


#endif

