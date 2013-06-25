#ifndef LITMUS_SRP_H
#define LITMUS_SRP_H

struct srp_semaphore;

struct srp_priority {
	struct list_head	list;
        unsigned int 		priority;
	pid_t			pid;
};
#define list2prio(l) list_entry(l, struct srp_priority, list)

/* struct for uniprocessor SRP "semaphore" */
struct srp_semaphore {
	struct litmus_lock litmus_lock;
	struct srp_priority ceiling;
	struct task_struct* owner;
	int cpu; /* cpu associated with this "semaphore" and resource */
};

/* map a task to its SRP preemption level priority */
typedef unsigned int (*srp_prioritization_t)(struct task_struct* t);
/* Must be updated by each plugin that uses SRP.*/
extern srp_prioritization_t get_srp_prio;

struct srp_semaphore* allocate_srp_semaphore(void);

#endif
