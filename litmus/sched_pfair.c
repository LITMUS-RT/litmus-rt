/*
 * kernel/sched_pfair.c
 *
 * Implementation of the (global) Pfair scheduling algorithm.
 *
 */

#include <asm/div64.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/list.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/rt_domain.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

#include <litmus/heap.h>

struct subtask {
	/* measured in quanta relative to job release */
	quanta_t release;
        quanta_t deadline;
	quanta_t overlap; /* called "b bit" by PD^2 */
	quanta_t group_deadline;
};

struct pfair_param   {
	quanta_t	quanta;       /* number of subtasks */
	quanta_t	cur;          /* index of current subtask */

	quanta_t	release;      /* in quanta */
	quanta_t	period;       /* in quanta */

	quanta_t	last_quantum; /* when scheduled last */
	int		last_cpu;     /* where scheduled last */

	unsigned int	present;    /* Can the task be scheduled? */
	unsigned int	sporadic_release; /* On wakeup, new sporadic release? */

	struct subtask subtasks[0];   /* allocate together with pfair_param */
};

#define tsk_pfair(tsk) ((tsk)->rt_param.pfair)

struct pfair_state {
	int cpu;
	volatile quanta_t cur_tick;    /* updated by the CPU that is advancing
				        * the time */
	volatile quanta_t local_tick;  /* What tick is the local CPU currently
				        * executing? Updated only by the local
				        * CPU. In QEMU, this may lag behind the
					* current tick. In a real system, with
					* proper timers and aligned quanta,
					* that should only be the
					* case for a very short time after the
					* time advanced. With staggered quanta,
					* it will lag for the duration of the
					* offset.
					*/

	struct task_struct* linked;    /* the task that should be executing */
	struct task_struct* local;     /* the local copy of linked          */
	struct task_struct* scheduled; /* what is actually scheduled        */

	unsigned long missed_quanta;
};

/* Currently, we limit the maximum period of any task to 1000 quanta.
 * The reason is that it makes the implementation easier since we do not
 * need to reallocate the release wheel on task arrivals.
 * In the future
 */
#define PFAIR_MAX_PERIOD 1000

/* This is the release queue wheel. It is indexed by pfair_time %
 * PFAIR_MAX_PERIOD.  Each heap is ordered by PFAIR priority, so that it can be
 * merged with the ready queue.
 */
static struct heap release_queue[PFAIR_MAX_PERIOD];

DEFINE_PER_CPU(struct pfair_state, pfair_state);
struct pfair_state*  pstate[NR_CPUS]; /* short cut */

#define NO_CPU 0xffffffff

static quanta_t pfair_time = 0; /* the "official" PFAIR clock */
static quanta_t merge_time = 0; /* Updated after the release queue has been
				 * merged. Used by drop_all_references().
				 */

static rt_domain_t pfair;

/* The pfair_lock is used to serialize all scheduling events.
 */
#define pfair_lock pfair.ready_lock

/* Enable for lots of trace info.
 * #define PFAIR_DEBUG
 */

#ifdef PFAIR_DEBUG
#define PTRACE_TASK(t, f, args...)  TRACE_TASK(t, f, # args)
#define PTRACE(f, args...) TRACE(f, # args)
#else
#define PTRACE_TASK(t, f, args...)
#define PTRACE(f, args...)
#endif

/* gcc will inline all of these accessor functions... */
static struct subtask* cur_subtask(struct task_struct* t)
{
	return tsk_pfair(t)->subtasks + tsk_pfair(t)->cur;
}

static quanta_t cur_deadline(struct task_struct* t)
{
	return cur_subtask(t)->deadline +  tsk_pfair(t)->release;
}

static quanta_t cur_release(struct task_struct* t)
{
#ifdef EARLY_RELEASE
	/* only the release of the first subtask counts when we early
	 * release */
	return tsk_pfair(t)->release;
#else
	return cur_subtask(t)->release +  tsk_pfair(t)->release;
#endif
}

static quanta_t cur_sub_release(struct task_struct* t)
{
	return cur_subtask(t)->release +  tsk_pfair(t)->release;
}

static quanta_t cur_overlap(struct task_struct* t)
{
	return cur_subtask(t)->overlap;
}

static quanta_t cur_group_deadline(struct task_struct* t)
{
	quanta_t gdl = cur_subtask(t)->group_deadline;
	if (gdl)
		return gdl + tsk_pfair(t)->release;
	else
		return gdl;
}

static int pfair_higher_prio(struct task_struct* first,
			     struct task_struct* second)
{
	return  /* first task must exist */
		first && (
		/* Does the second task exist and is it a real-time task?  If
		 * not, the first task (which is a RT task) has higher
		 * priority.
		 */
		!second || !is_realtime(second)  ||

		/* Is the (subtask) deadline of the first task earlier?
		 * Then it has higher priority.
		 */
		time_before(cur_deadline(first), cur_deadline(second)) ||

		/* Do we have a deadline tie?
		 * Then break by B-bit.
		 */
		(cur_deadline(first) == cur_deadline(second) &&
		 cur_overlap(first) > cur_overlap(second)) ||

		/* Do we have a B-bit tie?
		 * Then break by group deadline.
		 */
		(cur_overlap(first) == cur_overlap(second) &&
		 time_after(cur_group_deadline(first),
			    cur_group_deadline(second))) ||

		/* Do we have a group deadline tie?
		 * Then break by PID, which are unique.
		 */
		(cur_group_deadline(first) ==
		 cur_group_deadline(second) &&
		 first->pid < second->pid));
}

int pfair_ready_order(struct heap_node* a, struct heap_node* b)
{
	return pfair_higher_prio(heap2task(a), heap2task(b));
}

/* return the proper release queue for time t */
static struct heap* relq(quanta_t t)
{
	struct heap* rq = &release_queue[t % PFAIR_MAX_PERIOD];
	return rq;
}

static void prepare_release(struct task_struct* t, quanta_t at)
{
	tsk_pfair(t)->release    = at;
	tsk_pfair(t)->cur        = 0;
}

static void __pfair_add_release(struct task_struct* t, struct heap* queue)
{
	heap_insert(pfair_ready_order, queue,
		    tsk_rt(t)->heap_node);
}

static void pfair_add_release(struct task_struct* t)
{
	BUG_ON(heap_node_in_heap(tsk_rt(t)->heap_node));
	__pfair_add_release(t, relq(cur_release(t)));
}

/* pull released tasks from the release queue */
static void poll_releases(quanta_t time)
{
	__merge_ready(&pfair, relq(time));
	merge_time = time;
}

static void check_preempt(struct task_struct* t)
{
	int cpu = NO_CPU;
	if (tsk_rt(t)->linked_on != tsk_rt(t)->scheduled_on &&
	    tsk_pfair(t)->present) {
		/* the task can be scheduled and
		 * is not scheduled where it ought to be scheduled
		 */
		cpu = tsk_rt(t)->linked_on != NO_CPU ?
			tsk_rt(t)->linked_on         :
			tsk_rt(t)->scheduled_on;
		PTRACE_TASK(t, "linked_on:%d, scheduled_on:%d\n",
			   tsk_rt(t)->linked_on, tsk_rt(t)->scheduled_on);
		/* preempt */
		if (cpu == smp_processor_id())
			set_tsk_need_resched(current);
		else {
			smp_send_reschedule(cpu);
		}
	}
}

/* caller must hold pfair_lock */
static void drop_all_references(struct task_struct *t)
{
        int cpu;
        struct pfair_state* s;
        struct heap* q;
        if (heap_node_in_heap(tsk_rt(t)->heap_node)) {
                /* figure out what queue the node is in */
                if (time_before_eq(cur_release(t), merge_time))
                        q = &pfair.ready_queue;
                else
                        q = relq(cur_release(t));
                heap_delete(pfair_ready_order, q,
                            tsk_rt(t)->heap_node);
        }
        for (cpu = 0; cpu < NR_CPUS; cpu++) {
                s = &per_cpu(pfair_state, cpu);
                if (s->linked == t)
                        s->linked = NULL;
                if (s->local  == t)
                        s->local  = NULL;
                if (s->scheduled  == t)
                        s->scheduled = NULL;
        }
}

/* returns 1 if the task needs to go the release queue */
static int advance_subtask(quanta_t time, struct task_struct* t, int cpu)
{
	struct pfair_param* p = tsk_pfair(t);

	p->cur = (p->cur + 1) % p->quanta;
	TRACE_TASK(t, "on %d advanced to subtask %lu\n",
		   cpu,
		   p->cur);
	if (!p->cur) {
		prepare_for_next_period(t);
		if (tsk_pfair(t)->present) {
			/* we start a new job */
			get_rt_flags(t) = RT_F_RUNNING;
			p->release += p->period;
		} else {
			/* remove task from system until it wakes */
			drop_all_references(t);
			tsk_pfair(t)->sporadic_release = 1;
			return 0;
		}
	}
	return time_after(cur_release(t), time);
}

static void advance_subtasks(quanta_t time)
{
	int cpu, missed;
	struct task_struct* l;
	struct pfair_param* p;

	for_each_online_cpu(cpu) {
		l = pstate[cpu]->linked;
		missed = pstate[cpu]->linked != pstate[cpu]->local;
		if (l) {
			p = tsk_pfair(l);
			p->last_quantum = time;
			p->last_cpu     =  cpu;
			if (advance_subtask(time, l, cpu)) {
				pstate[cpu]->linked = NULL;
				pfair_add_release(l);
			}
		}
	}
}

static int target_cpu(quanta_t time, struct task_struct* t, int default_cpu)
{
	int cpu;
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		/* always observe scheduled_on linkage */
		default_cpu = tsk_rt(t)->scheduled_on;
		PTRACE_TASK(t, "forced on %d (scheduled on)\n", default_cpu);
	} else if (tsk_pfair(t)->last_quantum == time - 1) {
		/* back2back quanta */
		/* Only observe last_quantum if no scheduled_on is in the way.
		 * This should only kick in if a CPU missed quanta, and that
		 * *should* only happen in QEMU.
		 */
		cpu = tsk_pfair(t)->last_cpu;
		if (!pstate[cpu]->linked ||
		    tsk_rt(pstate[cpu]->linked)->scheduled_on != cpu) {
			default_cpu = cpu;
			PTRACE_TASK(t, "forced on %d (linked on)\n",
				    default_cpu);
		} else {
			PTRACE_TASK(t, "DID NOT force on %d (linked on)\n",
				    default_cpu);
		}
	}
	return default_cpu;
}

/* returns one if linking was redirected */
static int pfair_link(quanta_t time, int cpu,
		      struct task_struct* t)
{
	int target = target_cpu(time, t, cpu);
	struct task_struct* prev  = pstate[cpu]->linked;
	struct task_struct* other;

	PTRACE_TASK(t, "linked to %d for quantum %lu\n", target, time);
	if (target != cpu) {
		other = pstate[target]->linked;
		pstate[target]->linked = t;
		tsk_rt(t)->linked_on   = target;
		if (!other)
			/* linked ok, but reschedule this CPU */
			return 1;
		if (target < cpu) {
			/* link other to cpu instead */
			tsk_rt(other)->linked_on = cpu;
			pstate[cpu]->linked      = other;
			if (prev) {
				/* prev got pushed back into the ready queue */
				tsk_rt(prev)->linked_on = NO_CPU;
				__add_ready(&pfair, prev);
			}
			/* we are done with this cpu */
			return 0;
		} else {
			/* re-add other, it's original CPU was not considered yet */
			tsk_rt(other)->linked_on = NO_CPU;
			__add_ready(&pfair, other);
			/* reschedule this CPU */
			return 1;
		}
	} else {
		pstate[cpu]->linked  = t;
		tsk_rt(t)->linked_on = cpu;
		if (prev) {
			/* prev got pushed back into the ready queue */
			tsk_rt(prev)->linked_on = NO_CPU;
			__add_ready(&pfair, prev);
		}
		/* we are done with this CPU */
		return 0;
	}
}

static void schedule_subtasks(quanta_t time)
{
	int cpu, retry;

	for_each_online_cpu(cpu) {
		retry = 1;
		while (retry) {
			if (pfair_higher_prio(__peek_ready(&pfair),
					      pstate[cpu]->linked))
				retry = pfair_link(time, cpu,
						   __take_ready(&pfair));
			else
				retry = 0;
		}
	}
}

static void schedule_next_quantum(quanta_t time)
{
	int cpu;

	PTRACE("<<< Q %lu at %llu\n",
	       time, litmus_clock());

	/* called with interrupts disabled */
	spin_lock(&pfair_lock);

	advance_subtasks(time);
	poll_releases(time);
	schedule_subtasks(time);

	spin_unlock(&pfair_lock);

	/* We are done. Advance time. */
	mb();
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		pstate[cpu]->cur_tick = pfair_time;
	PTRACE(">>> Q %lu at %llu\n",
	       time, litmus_clock());
}

/* pfair_tick - this function is called for every local timer
 *                         interrupt.
 */
static void pfair_tick(struct task_struct* t)
{
	struct pfair_state* state = &__get_cpu_var(pfair_state);
	quanta_t time, loc, cur;

	/* Attempt to advance time. First CPU to get here 00
	 * will prepare the next quantum.
	 */
	time = cmpxchg(&pfair_time,
		       state->local_tick,     /* expected */
		       state->local_tick + 1  /* next     */
		);
	if (time == state->local_tick)
		/* exchange succeeded */
		schedule_next_quantum(time + 1);

	/* Spin locally until time advances. */
	while (1) {
		mb();
		cur = state->cur_tick;
		loc = state->local_tick;
		if (time_before(loc, cur)) {
			if (loc + 1 != cur) {
				TRACE("MISSED quantum! loc:%lu -> cur:%lu\n",
				      loc, cur);
				state->missed_quanta++;
			}
			break;
		}
		cpu_relax();
	}

	/* copy state info */
	state->local_tick = state->cur_tick;
	state->local      = state->linked;
	if (state->local && tsk_pfair(state->local)->present &&
	    state->local != current)
		set_tsk_need_resched(current);
}

static int safe_to_schedule(struct task_struct* t, int cpu)
{
	int where = tsk_rt(t)->scheduled_on;
	if (where != NO_CPU && where != cpu) {
		TRACE_TASK(t, "BAD: can't be scheduled on %d, "
			   "scheduled already on %d.\n", cpu, where);
		return 0;
	} else
		return tsk_pfair(t)->present && get_rt_flags(t) == RT_F_RUNNING;
}

static struct task_struct* pfair_schedule(struct task_struct * prev)
{
	struct pfair_state* state = &__get_cpu_var(pfair_state);
	int blocks;
	struct task_struct* next = NULL;

	spin_lock(&pfair_lock);

	blocks  = is_realtime(prev) && !is_running(prev);

	if (blocks)
		tsk_pfair(prev)->present = 0;

	if (state->local && safe_to_schedule(state->local, state->cpu))
		next = state->local;

	if (prev != next) {
		tsk_rt(prev)->scheduled_on = NO_CPU;
		if (next)
			tsk_rt(next)->scheduled_on = state->cpu;
	}

	spin_unlock(&pfair_lock);

	if (next)
		TRACE_TASK(next, "scheduled rel=%lu at %lu\n",
			   tsk_pfair(next)->release, pfair_time);
	else if (is_realtime(prev))
		TRACE("Becomes idle at %lu\n", pfair_time);

	return next;
}

static void pfair_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;

	TRACE("pfair: task new %d state:%d\n", t->pid, t->state);

	spin_lock_irqsave(&pfair_lock, flags);
	if (running)
		t->rt_param.scheduled_on = task_cpu(t);
	else
		t->rt_param.scheduled_on = NO_CPU;

	prepare_release(t, pfair_time + 1);
	tsk_pfair(t)->present = running;
	tsk_pfair(t)->sporadic_release = 0;
	pfair_add_release(t);
	check_preempt(t);

	spin_unlock_irqrestore(&pfair_lock, flags);
}

static void pfair_task_wake_up(struct task_struct *t)
{
	unsigned long flags;

	TRACE_TASK(t, "wakes at %lld, release=%lu, pfair_time:%lu\n",
		   cur_release(t), pfair_time);

	spin_lock_irqsave(&pfair_lock, flags);

	tsk_pfair(t)->present = 1;

	/* It is a little unclear how to deal with Pfair
	 * tasks that block for a while and then wake. For now,
	 * if a task blocks and wakes before its next job release,
	 * then it may resume if it is currently linked somewhere
	 * (as if it never blocked at all). Otherwise, we have a
	 * new sporadic job release.
	 */
	if (tsk_pfair(t)->sporadic_release) {
		prepare_release(t, pfair_time + 1);
		pfair_add_release(t);
		tsk_pfair(t)->sporadic_release = 0;
	}

	check_preempt(t);

	spin_unlock_irqrestore(&pfair_lock, flags);
}

static void pfair_task_block(struct task_struct *t)
{
	BUG_ON(!is_realtime(t));
	TRACE_TASK(t, "blocks at %lld, state:%d\n",
		   (lt_t) jiffies, t->state);
}

static void pfair_task_exit(struct task_struct * t)
{
	unsigned long flags;

	BUG_ON(!is_realtime(t));

	/* Remote task from release or ready queue, and ensure
	 * that it is not the scheduled task for ANY CPU. We
	 * do this blanket check because occassionally when
	 * tasks exit while blocked, the task_cpu of the task
	 * might not be the same as the CPU that the PFAIR scheduler
	 * has chosen for it.
	 */
	spin_lock_irqsave(&pfair_lock, flags);

	TRACE_TASK(t, "RIP, state:%d\n", t->state);
	drop_all_references(t);

	spin_unlock_irqrestore(&pfair_lock, flags);

	kfree(t->rt_param.pfair);
	t->rt_param.pfair = NULL;
}


static void pfair_release_at(struct task_struct* task, lt_t start)
{
	unsigned long flags;
	lt_t now = litmus_clock();
	quanta_t release, delta;

	BUG_ON(!is_realtime(task));

	spin_lock_irqsave(&pfair_lock, flags);
	if (lt_before(now, start)) {
		delta = time2quanta((long long) start - (long long) now, CEIL);
		if (delta >= PFAIR_MAX_PERIOD)
			delta = PFAIR_MAX_PERIOD - 1;
	} else
		delta = 10;  /* release in 10 ticks */

	release = pfair_time + delta;

	drop_all_references(task);
	prepare_release(task, release);
	pfair_add_release(task);

	/* Clear sporadic release flag, since this release subsumes any
	 * sporadic release on wake.
	 */
	tsk_pfair(task)->sporadic_release = 0;

	spin_unlock_irqrestore(&pfair_lock, flags);
}

static void init_subtask(struct subtask* sub, unsigned long i,
			 lt_t quanta, lt_t period)
{
	/* since i is zero-based, the formulas are shifted by one */
	lt_t tmp;

	/* release */
	tmp = period * i;
	do_div(tmp, quanta); /* floor */
	sub->release = (quanta_t) tmp;

	/* deadline */
	tmp = period * (i + 1);
	if (do_div(tmp, quanta)) /* ceil */
		tmp++;
	sub->deadline = (quanta_t) tmp;

	/* next release */
	tmp = period * (i + 1);
	do_div(tmp, quanta); /* floor */
	sub->overlap =  sub->deadline - (quanta_t) tmp;

	/* Group deadline.
	 * Based on the formula given in Uma's thesis.
	 */
	if (2 * quanta >= period) {
		/* heavy */
		tmp = (sub->deadline - (i + 1)) * period;
		if (do_div(tmp, (period - quanta))) /* ceil */
			tmp++;
		sub->group_deadline = (quanta_t) tmp;
	} else
		sub->group_deadline = 0;
}

static void dump_subtasks(struct task_struct* t)
{
	unsigned long i;
	for (i = 0; i < t->rt_param.pfair->quanta; i++)
		TRACE_TASK(t, "SUBTASK %lu: rel=%lu dl=%lu bbit:%lu gdl:%lu\n",
			   i + 1,
			   t->rt_param.pfair->subtasks[i].release,
			   t->rt_param.pfair->subtasks[i].deadline,
			   t->rt_param.pfair->subtasks[i].overlap,
			   t->rt_param.pfair->subtasks[i].group_deadline);
}

static long pfair_admit_task(struct task_struct* t)
{
	lt_t quanta;
	lt_t period;
	s64  quantum_length = ktime_to_ns(tick_period);
	struct pfair_param* param;
	unsigned long i;

	/* Pfair is a tick-based method, so the time
	 * of interest is jiffies. Calculate tick-based
	 * times for everything.
	 * (Ceiling of exec cost, floor of period.)
	 */

	quanta = get_exec_cost(t);
	period = get_rt_period(t);

	quanta = time2quanta(get_exec_cost(t), CEIL);

	if (do_div(period, quantum_length))
		printk(KERN_WARNING
		       "The period of %s/%d is not a multiple of %llu.\n",
		       t->comm, t->pid, (unsigned long long) quantum_length);

	if (period >= PFAIR_MAX_PERIOD) {
		printk(KERN_WARNING
		       "PFAIR: Rejecting task %s/%d; its period is too long.\n",
		       t->comm, t->pid);
		return -EINVAL;
	}

	param = kmalloc(sizeof(struct pfair_param) +
			quanta * sizeof(struct subtask), GFP_ATOMIC);

	if (!param)
		return -ENOMEM;

	param->quanta  = quanta;
	param->cur     = 0;
	param->release = 0;
	param->period  = period;

	for (i = 0; i < quanta; i++)
		init_subtask(param->subtasks + i, i, quanta, period);

	if (t->rt_param.pfair)
		/* get rid of stale allocation */
		kfree(t->rt_param.pfair);

	t->rt_param.pfair = param;

	/* spew out some debug info */
	dump_subtasks(t);

	return 0;
}

/*	Plugin object	*/
static struct sched_plugin pfair_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "PFAIR",
	.tick			= pfair_tick,
	.task_new		= pfair_task_new,
	.task_exit		= pfair_task_exit,
	.schedule		= pfair_schedule,
	.task_wake_up		= pfair_task_wake_up,
	.task_block		= pfair_task_block,
	.admit_task		= pfair_admit_task,
	.release_at		= pfair_release_at,
	.complete_job		= complete_job
};

static int __init init_pfair(void)
{
	int cpu, i;
	struct pfair_state *state;

	/* initialize release queue */
	for (i = 0; i < PFAIR_MAX_PERIOD; i++)
		heap_init(&release_queue[i]);

	/* initialize CPU state */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		state = &per_cpu(pfair_state, cpu);
		state->cpu 	  = cpu;
		state->cur_tick   = 0;
		state->local_tick = 0;
		state->linked     = NULL;
		state->local      = NULL;
		state->scheduled  = NULL;
		state->missed_quanta = 0;
		pstate[cpu] = state;
	}

	rt_domain_init(&pfair, pfair_ready_order, NULL, NULL);
	return register_sched_plugin(&pfair_plugin);
}

module_init(init_pfair);

