/*
 * Definition of the scheduler plugin interface.
 *
 */
#ifndef _LINUX_SCHED_PLUGIN_H_
#define _LINUX_SCHED_PLUGIN_H_

#include <linux/sched.h>

#ifdef CONFIG_LITMUS_LOCKING
#include <litmus/locking.h>
#endif

/************************ setup/tear down ********************/

typedef long (*activate_plugin_t) (void);
typedef long (*deactivate_plugin_t) (void);

struct domain_proc_info;
typedef long (*get_domain_proc_info_t) (struct domain_proc_info **info);


/********************* scheduler invocation ******************/
/* The main scheduling function, called to select the next task to dispatch. */
typedef struct task_struct* (*schedule_t)(struct task_struct * prev);
/* Clean up after the task switch has occured.
 * This function is called after every (even non-rt) task switch.
 */
typedef void (*finish_switch_t)(struct task_struct *prev);


/* When waiting for the stack of the task selected by the plugin
 * to become available, this callback is invoked to give the
 * plugin a chance to cancel the wait. If the plugin returns false,
 * the scheduler is invoked again. */
typedef bool (*should_wait_for_stack_t)(struct task_struct *next);

/* After a pull migration (which involves dropping scheduler locks),
 * the plugin is given the chance to validate that the task is still
 * the right one. If the plugin returns false, the scheduler
 * will be invoked again. */
typedef bool (*post_migration_validate_t)(struct task_struct *next);

/* After dropping the lock to facilitate a pull migration, the task
 * state may have changed. In this case, the core notifies the plugin
 * with this callback and then invokes the scheduler again. */
typedef void (*next_became_invalid_t)(struct task_struct *next);

/********************* task state changes ********************/

/* Called to setup a new real-time task.
 * Release the first job, enqueue, etc.
 * Task may already be running.
 */
typedef void (*task_new_t) (struct task_struct *task,
			    int on_rq,
			    int running);

/* Called when userspace seeks to set new task parameters for a task
 * that is already in real-time mode (i.e., is_realtime(task)).
 */
typedef long (*task_change_params_t) (struct task_struct *task,
			    struct rt_task *new_params);

/* Called to re-introduce a task after blocking.
 * Can potentially be called multiple times.
 */
typedef void (*task_wake_up_t) (struct task_struct *task);
/* called to notify the plugin of a blocking real-time task
 * it will only be called for real-time tasks and before schedule is called */
typedef void (*task_block_t)  (struct task_struct *task);
/* Called when a real-time task exits or changes to a different scheduling
 * class.
 * Free any allocated resources
 */
typedef void (*task_exit_t)    (struct task_struct *);

/* task_exit() is called with interrupts disabled and runqueue locks held, and
 * thus and cannot block or spin.  task_cleanup() is called sometime later
 * without any locks being held.
 */
typedef void (*task_cleanup_t)	(struct task_struct *);

#ifdef CONFIG_LITMUS_LOCKING
/* Called when the current task attempts to create a new lock of a given
 * protocol type. */
typedef long (*allocate_lock_t) (struct litmus_lock **lock, int type,
				 void* __user config);
#endif


/********************* sys call backends  ********************/
/* This function causes the caller to sleep until the next release */
typedef long (*complete_job_t) (void);

typedef long (*admit_task_t)(struct task_struct* tsk);

/* return false to indicate that the plugin does not support forking */
typedef bool (*fork_task_t)(struct task_struct* tsk);

typedef long (*wait_for_release_at_t)(lt_t release_time);

/* Informs the plugin when a synchronous release takes place. */
typedef void (*synchronous_release_at_t)(lt_t time_zero);

/* How much budget has the current task consumed so far, and how much
 * has it left? The default implementation ties into the per-task
 * budget enforcement code. Plugins can override this to report
 * reservation-specific values. */
typedef void (*current_budget_t)(lt_t *used_so_far, lt_t *remaining);

/* Reservation creation/removal backends. Meaning of reservation_type and
 * reservation_id are entirely plugin-specific. */
typedef long (*reservation_create_t)(int reservation_type, void* __user config);
typedef long (*reservation_destroy_t)(unsigned int reservation_id, int cpu);

/************************ misc routines ***********************/


struct sched_plugin {
	struct list_head	list;
	/* 	basic info 		*/
	char 			*plugin_name;

	/*	setup			*/
	activate_plugin_t	activate_plugin;
	deactivate_plugin_t	deactivate_plugin;
	get_domain_proc_info_t	get_domain_proc_info;

	/* 	scheduler invocation 	*/
	schedule_t 		schedule;
	finish_switch_t 	finish_switch;

	/* control over pull migrations */
	should_wait_for_stack_t should_wait_for_stack;
	next_became_invalid_t next_became_invalid;
	post_migration_validate_t post_migration_validate;

	/*	syscall backend 	*/
	complete_job_t 		complete_job;
	wait_for_release_at_t	wait_for_release_at;
	synchronous_release_at_t synchronous_release_at;

	/*	task state changes 	*/
	admit_task_t		admit_task;
	fork_task_t			fork_task;

    task_new_t 			task_new;
	task_wake_up_t		task_wake_up;
	task_block_t		task_block;

	/* optional: support task parameter changes at runtime */
    task_change_params_t task_change_params;

	task_exit_t 		task_exit;
	task_cleanup_t		task_cleanup;

	current_budget_t	current_budget;

	/* Reservation support */
	reservation_create_t	reservation_create;
	reservation_destroy_t	reservation_destroy;

#ifdef CONFIG_LITMUS_LOCKING
	/*	locking protocols	*/
	allocate_lock_t		allocate_lock;
#endif
} __attribute__ ((__aligned__(SMP_CACHE_BYTES)));


extern struct sched_plugin *litmus;

int register_sched_plugin(struct sched_plugin* plugin);
struct sched_plugin* find_sched_plugin(const char* name);
void print_sched_plugins(struct seq_file *m);


extern struct sched_plugin linux_sched_plugin;

#endif
