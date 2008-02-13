/* EDF common data structures and utility functions shared by all EDF
 * based scheduler plugins
 */

/* CLEANUP: Add comments and make it less messy.
 *
 */

#ifndef __UNC_EDF_COMMON_H__
#define __UNC_EDF_COMMON_H__

#include <litmus/rt_domain.h>


void edf_domain_init(rt_domain_t* rt, check_resched_needed_t resched);

int edf_higher_prio(struct task_struct* first,
		    struct task_struct* second);

int edf_ready_order(struct list_head* a, struct list_head* b);

void edf_release_at(struct task_struct *t, lt_t start);

int  edf_preemption_needed(rt_domain_t* rt, struct task_struct *t);
long edf_complete_job(void);

void edf_prepare_for_next_period(struct task_struct *t);

#define job_completed(t) (!is_be(t) && \
	(t)->rt_param.times.exec_time == (t)->rt_param.basic_params.exec_cost)

int edf_set_hp_task(struct pi_semaphore *sem);
int edf_set_hp_cpu_task(struct pi_semaphore *sem, int cpu);

#endif
