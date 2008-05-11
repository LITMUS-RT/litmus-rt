/*
 * kernel/rt_domain.c
 *
 * LITMUS real-time infrastructure. This file contains the
 * functions that manipulate RT domains. RT domains are an abstraction
 * of a ready queue and a release queue.
 */

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

#include <litmus/rt_domain.h>


static int dummy_resched(rt_domain_t *rt)
{
	return 0;
}

static int dummy_order(struct list_head* a, struct list_head* b)
{
	return 0;
}

/* default implementation: use default lock */
static void default_release_job(struct task_struct* t, rt_domain_t* rt)
{
	add_ready(rt, t);
}

static enum hrtimer_restart release_job_timer(struct hrtimer *timer)
{
	struct task_struct *t;
	
	t = container_of(timer, struct task_struct, 
			 rt_param.release_timer);

	get_domain(t)->release_job(t, get_domain(t));

	return HRTIMER_NORESTART;
}

static void setup_job_release_timer(struct task_struct *task)
{
        hrtimer_init(&release_timer(task), CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
        release_timer(task).function = release_job_timer;
#ifdef CONFIG_HIGH_RES_TIMERS
        release_timer(task).cb_mode = HRTIMER_CB_IRQSAFE_NO_RESTART;
#endif
        /* Expiration time of timer is release time of task. */
	release_timer(task).expires = ns_to_ktime(get_release(task));

	TRACE_TASK(task, "arming release timer rel=%llu at %llu\n",
		   get_release(task), litmus_clock());

	hrtimer_start(&release_timer(task), release_timer(task).expires,
		      HRTIMER_MODE_ABS);
}

static void arm_release_timers(unsigned long _rt)
{
	rt_domain_t *rt = (rt_domain_t*) _rt;
	unsigned long flags;
	struct list_head alt;
	struct list_head *pos, *safe;
	struct task_struct* t;

	spin_lock_irqsave(&rt->release_lock, flags);
	list_replace_init(&rt->release_queue, &alt);
	spin_unlock_irqrestore(&rt->release_lock, flags);
	
	list_for_each_safe(pos, safe, &alt) {
		t = list_entry(pos, struct task_struct, rt_list);
		list_del(pos);
		setup_job_release_timer(t);
	}
}


void rt_domain_init(rt_domain_t *rt,
		    list_cmp_t order,
		    check_resched_needed_t check,
		    release_job_t release
		   )
{
	BUG_ON(!rt);
	if (!check)
		check = dummy_resched;
	if (!release)
		release = default_release_job;
	if (!order)
		order = dummy_order;
	INIT_LIST_HEAD(&rt->ready_queue);
	INIT_LIST_HEAD(&rt->release_queue);
	spin_lock_init(&rt->ready_lock);
	spin_lock_init(&rt->release_lock);
	rt->check_resched 	= check;
	rt->release_job		= release;
	rt->order		= order;
	init_no_rqlock_work(&rt->arm_timers, arm_release_timers, (unsigned long) rt);
}

/* add_ready - add a real-time task to the rt ready queue. It must be runnable.
 * @new:      the newly released task
 */
void __add_ready(rt_domain_t* rt, struct task_struct *new)
{
	TRACE("rt: adding %s/%d (%llu, %llu) rel=%llu to ready queue at %llu\n",
	      new->comm, new->pid, get_exec_cost(new), get_rt_period(new),
	      get_release(new), litmus_clock());

	if (!list_insert(&new->rt_list, &rt->ready_queue, rt->order))
		rt->check_resched(rt);
}

struct task_struct* __take_ready(rt_domain_t* rt)
{
	struct task_struct *t = __peek_ready(rt);

	/* kick it out of the ready list */
	if (t)
		list_del(&t->rt_list);
	return t;
}

struct task_struct* __peek_ready(rt_domain_t* rt)
{
	if (!list_empty(&rt->ready_queue))
		return next_ready(rt);
	else
		return NULL;
}

/* add_release - add a real-time task to the rt release queue.
 * @task:        the sleeping task
 */
void __add_release(rt_domain_t* rt, struct task_struct *task)
{
	TRACE_TASK(task, "add_release(), rel=%llu\n", get_release(task));
	list_add(&task->rt_list, &rt->release_queue);
	task->rt_param.domain = rt;
	do_without_rqlock(&rt->arm_timers);
}

