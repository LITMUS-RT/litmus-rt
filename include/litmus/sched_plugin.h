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



/********************* scheduler invocation ******************/

/*  Plugin-specific realtime tick handler */
typedef void (*scheduler_tick_t) (struct task_struct *cur);
/* Novell make sched decision function */
typedef struct task_struct* (*schedule_t)(struct task_struct * prev);
/* Clean up after the task switch has occured.
 * This function is called after every (even non-rt) task switch.
 */
typedef void (*finish_switch_t)(struct task_struct *prev);


/********************* task state changes ********************/

/* Called to setup a new real-time task.
 * Release the first job, enqueue, etc.
 * Task may already be running.
 */
typedef void (*task_new_t) (struct task_struct *task,
			    int on_rq,
			    int running);

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

typedef void (*release_at_t)(struct task_struct *t, lt_t start);

struct sched_plugin {
	struct list_head	list;
	/* 	basic info 		*/
	char 			*plugin_name;

	/*	setup			*/
	activate_plugin_t	activate_plugin;
	deactivate_plugin_t	deactivate_plugin;

	/* 	scheduler invocation 	*/
	scheduler_tick_t        tick;
	schedule_t 		schedule;
	finish_switch_t 	finish_switch;

	/*	syscall backend 	*/
	complete_job_t 		complete_job;
	release_at_t		release_at;

	/*	task state changes 	*/
	admit_task_t		admit_task;

        task_new_t 		task_new;
	task_wake_up_t		task_wake_up;
	task_block_t		task_block;

	task_exit_t 		task_exit;
	task_cleanup_t		task_cleanup;

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
