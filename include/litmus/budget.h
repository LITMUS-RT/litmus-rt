#ifndef _LITMUS_BUDGET_H_
#define _LITMUS_BUDGET_H_

/* Update the per-processor enforcement timer (arm/reproram/cancel) for
 * the next task. */
void update_enforcement_timer(struct task_struct* t);

#endif
