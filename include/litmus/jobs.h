#ifndef __LITMUS_JOBS_H__
#define __LITMUS_JOBS_H__

void prepare_for_next_period(struct task_struct *t);
void release_at(struct task_struct *t, lt_t start);

long default_wait_for_release_at(lt_t release_time);
long complete_job(void);

#endif
