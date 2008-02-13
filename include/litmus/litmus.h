/*
 * Constant definitions related to
 * scheduling policy.
 */

#ifndef _LINUX_LITMUS_H_
#define _LINUX_LITMUS_H_

#include <linux/jiffies.h>
#include <litmus/sched_trace.h>

typedef enum {
	SCHED_LINUX 		=  0,
	SCHED_GSN_EDF		= 10,
	SCHED_PSN_EDF		= 11,
	/*      Add your scheduling policy here */

	SCHED_DEFAULT 		=  0,
	SCHED_INVALID 		= -1,
} spolicy;


typedef enum {
	LITMUS_RESERVED_RANGE = 1024,

} sched_setup_cmd_t;

/*	per-task modes */
enum rt_task_mode_t {
	BACKGROUND_TASK = 0,
	LITMUS_RT_TASK  = 1
};

/*	Plugin boot options, for convenience */
#define PLUGIN_LINUX  		"linux"
#define PLUGIN_GSN_EDF		"gsn_edf"
#define PLUGIN_PSN_EDF		"psn_edf"

extern spolicy sched_policy;

/*	RT mode start time	*/
extern volatile unsigned long rt_start_time;

#define TRACE(fmt, args...) \
	sched_trace_log_message("%d: " fmt, raw_smp_processor_id(), ## args)

#define TRACE_TASK(t, fmt, args...) \
	TRACE("(%s/%d) " fmt, (t)->comm, (t)->pid, ##args)

#define TRACE_CUR(fmt, args...) \
	TRACE_TASK(current, fmt, ## args)

#define TRACE_BUG_ON(cond) \
	do { if (cond) TRACE("BUG_ON(%s) at %s:%d " \
			     "called from %p current=%s/%d state=%d " \
			     "flags=%x partition=%d cpu=%d rtflags=%d"\
			     " job=%u knp=%d timeslice=%u\n",		\
	#cond, __FILE__, __LINE__, __builtin_return_address(0), current->comm, \
	current->pid, current->state, current->flags,  \
	get_partition(current), smp_processor_id(), get_rt_flags(current), \
	current->rt_param.job_params.job_no, current->rt_param.kernel_np, \
	current->time_slice\
	); } while(0);


/* in_list - is a given list_head queued on some list?
 */
static inline int in_list(struct list_head* list)
{
	return !(  /* case 1: deleted */
		   (list->next == LIST_POISON1 &&
		    list->prev == LIST_POISON2)
		 ||
		   /* case 2: initialized */
		   (list->next == list &&
		    list->prev == list)
		);
}

typedef int (*list_cmp_t)(struct list_head*, struct list_head*);

static inline unsigned int list_insert(struct list_head* new,
				       struct list_head* head,
				       list_cmp_t order_before)
{
	struct list_head *pos;
	unsigned int passed = 0;

	BUG_ON(!new);

	/* find a spot where the new entry is less than the next */
	list_for_each(pos, head) {
		if (unlikely(order_before(new, pos))) {
			/* pos is not less than new, thus insert here */
			__list_add(new, pos->prev, pos);
			goto out;
		}
		passed++;
	}
	/* if we get to this point either the list is empty or every entry
	 * queued element is less than new.
	 * Let's add new to the end. */
	list_add_tail(new, head);
 out:
	return passed;
}

void list_qsort(struct list_head* list, list_cmp_t less_than);


#define RT_PREEMPTIVE 		0x2050 /* = NP */
#define RT_NON_PREEMPTIVE 	0x4e50 /* =  P */
#define RT_EXIT_NP_REQUESTED	0x5251 /* = RQ */

/* returns 1 if task t has registered np flag and set it to RT_NON_PREEMPTIVE
 */
int is_np(struct task_struct *t);

/* request that the task should call sys_exit_np()
 */
void request_exit_np(struct task_struct *t);

/* kill naughty tasks
 */
void scheduler_signal(struct task_struct *t, unsigned int signal);
void send_scheduler_signals(void);
void np_mem_kill(struct task_struct *t);

void litmus_fork(struct task_struct *tsk);
void litmus_exec(void);
/* clean up real-time state of a task */
void exit_litmus(struct task_struct *dead_tsk);

long litmus_admit_task(struct task_struct *tsk);
void litmus_exit_task(struct task_struct *tsk);

#define is_realtime(t) 		((t)->policy == SCHED_LITMUS)
#define rt_transition_pending(t) \
	((t)->rt_param.transition_pending)

/*	Realtime utility macros */
#define get_rt_flags(t)		((t)->rt_param.flags)
#define set_rt_flags(t,f) 	(t)->rt_param.flags=(f)
#define get_exec_cost(t)  	((t)->rt_param.task_params.exec_cost)
#define get_exec_time(t)	((t)->rt_param.job_params.exec_time)
#define get_rt_period(t)	((t)->rt_param.task_params.period)
#define get_partition(t) 	(t)->rt_param.task_params.cpu
#define get_deadline(t)		((t)->rt_param.job_params.deadline)
#define get_class(t)		((t)->rt_param.task_params.cls)

inline static int budget_exhausted(struct task_struct* t)
{
	return get_exec_time(t) >= get_exec_cost(t);
}

#define is_subject_to_srp(t)	((t)->rt_param.subject_to_srp)
#define is_hrt(t)     		\
	((t)->rt_param.task_params.class == RT_CLASS_HARD)
#define is_srt(t)     		\
	((t)->rt_param.task_params.class == RT_CLASS_SOFT)
#define is_be(t)      		\
	((t)->rt_param.task_params.class == RT_CLASS_BEST_EFFORT)

#define get_release(t) ((t)->rt_param.job_params.release)

/* Honor the flag in the preempt_count variable that is set
 * when scheduling is in progress.
 */
#define is_running(t) 			\
	((t)->state == TASK_RUNNING || 	\
	 task_thread_info(t)->preempt_count & PREEMPT_ACTIVE)

#define is_blocked(t)       \
	(!is_running(t))
#define is_released(t, now)	\
	(lt_before_eq(get_release(t), now))
#define is_tardy(t, now)    \
	(lt_before_eq((t)->rt_param.job_params.deadline, now))

/* real-time comparison macros */
#define earlier_deadline(a, b) (lt_before(\
	(a)->rt_param.job_params.deadline,\
	(b)->rt_param.job_params.deadline))
#define earlier_release(a, b)  (lt_before(\
	(a)->rt_param.job_params.release,\
	(b)->rt_param.job_params.release))

#define make_np(t) do {t->rt_param.kernel_np++;} while(0);
#define take_np(t) do {t->rt_param.kernel_np--;} while(0);


#endif
