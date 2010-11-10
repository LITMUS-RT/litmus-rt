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
#include <linux/slab.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/preempt.h>
#include <litmus/rt_domain.h>
#include <litmus/sched_plugin.h>
#include <litmus/sched_trace.h>

#include <litmus/bheap.h>

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
	lt_t offset;			/* stagger offset */
};

/* Currently, we limit the maximum period of any task to 2000 quanta.
 * The reason is that it makes the implementation easier since we do not
 * need to reallocate the release wheel on task arrivals.
 * In the future
 */
#define PFAIR_MAX_PERIOD 2000

/* This is the release queue wheel. It is indexed by pfair_time %
 * PFAIR_MAX_PERIOD.  Each heap is ordered by PFAIR priority, so that it can be
 * merged with the ready queue.
 */
static struct bheap release_queue[PFAIR_MAX_PERIOD];

DEFINE_PER_CPU(struct pfair_state, pfair_state);
struct pfair_state* *pstate; /* short cut */

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
#define PTRACE_TASK(t, f, args...)  TRACE_TASK(t, f, ## args)
#define PTRACE(f, args...) TRACE(f, ## args)
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


static quanta_t cur_sub_release(struct task_struct* t)
{
	return cur_subtask(t)->release +  tsk_pfair(t)->release;
}

static quanta_t cur_release(struct task_struct* t)
{
#ifdef EARLY_RELEASE
	/* only the release of the first subtask counts when we early
	 * release */
	return tsk_pfair(t)->release;
#else
	return cur_sub_release(t);
#endif
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
		 (cur_overlap(first) > cur_overlap(second) ||

		/* Do we have a B-bit tie?
		 * Then break by group deadline.
		 */
		(cur_overlap(first) == cur_overlap(second) &&
		 (time_after(cur_group_deadline(first),
			     cur_group_deadline(second)) ||

		/* Do we have a group deadline tie?
		 * Then break by PID, which are unique.
		 */
		(cur_group_deadline(first) ==
		 cur_group_deadline(second) &&
		 first->pid < second->pid))))));
}

int pfair_ready_order(struct bheap_node* a, struct bheap_node* b)
{
	return pfair_higher_prio(bheap2task(a), bheap2task(b));
}

/* return the proper release queue for time t */
static struct bheap* relq(quanta_t t)
{
	struct bheap* rq = &release_queue[t % PFAIR_MAX_PERIOD];
	return rq;
}

static void prepare_release(struct task_struct* t, quanta_t at)
{
	tsk_pfair(t)->release    = at;
	tsk_pfair(t)->cur        = 0;
}

static void __pfair_add_release(struct task_struct* t, struct bheap* queue)
{
	bheap_insert(pfair_ready_order, queue,
		    tsk_rt(t)->heap_node);
}

static void pfair_add_release(struct task_struct* t)
{
	BUG_ON(bheap_node_in_heap(tsk_rt(t)->heap_node));
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
	    tsk_rt(t)->present) {
		/* the task can be scheduled and
		 * is not scheduled where it ought to be scheduled
		 */
		cpu = tsk_rt(t)->linked_on != NO_CPU ?
			tsk_rt(t)->linked_on         :
			tsk_rt(t)->scheduled_on;
		PTRACE_TASK(t, "linked_on:%d, scheduled_on:%d\n",
			   tsk_rt(t)->linked_on, tsk_rt(t)->scheduled_on);
		/* preempt */
		litmus_reschedule(cpu);
	}
}

/* caller must hold pfair_lock */
static void drop_all_references(struct task_struct *t)
{
        int cpu;
        struct pfair_state* s;
        struct bheap* q;
        if (bheap_node_in_heap(tsk_rt(t)->heap_node)) {
                /* figure out what queue the node is in */
                if (time_before_eq(cur_release(t), merge_time))
                        q = &pfair.ready_queue;
                else
                        q = relq(cur_release(t));
                bheap_delete(pfair_ready_order, q,
                            tsk_rt(t)->heap_node);
        }
        for (cpu = 0; cpu < num_online_cpus(); cpu++) {
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
	int to_relq;
	p->cur = (p->cur + 1) % p->quanta;
	if (!p->cur) {
		sched_trace_task_completion(t, 1);
		if (tsk_rt(t)->present) {
			/* we start a new job */
			prepare_for_next_period(t);
			sched_trace_task_release(t);
			get_rt_flags(t) = RT_F_RUNNING;
			p->release += p->period;
		} else {
			/* remove task from system until it wakes */
			drop_all_references(t);
			tsk_pfair(t)->sporadic_release = 1;
			TRACE_TASK(t, "on %d advanced to subtask %lu (not present)\n",
				   cpu, p->cur);
			return 0;
		}
	}
	to_relq = time_after(cur_release(t), time);
	TRACE_TASK(t, "on %d advanced to subtask %lu -> to_relq=%d\n",
		   cpu, p->cur, to_relq);
	return to_relq;
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

	/* called with interrupts disabled */
	PTRACE("--- Q %lu at %llu PRE-SPIN\n",
	       time, litmus_clock());
	raw_spin_lock(&pfair_lock);
	PTRACE("<<< Q %lu at %llu\n",
	       time, litmus_clock());

	sched_trace_quantum_boundary();

	advance_subtasks(time);
	poll_releases(time);
	schedule_subtasks(time);

	for (cpu = 0; cpu < num_online_cpus(); cpu++)
		if (pstate[cpu]->linked)
			PTRACE_TASK(pstate[cpu]->linked,
				    " linked on %d.\n", cpu);
		else
			PTRACE("(null) linked on %d.\n", cpu);

	/* We are done. Advance time. */
	mb();
	for (cpu = 0; cpu < num_online_cpus(); cpu++) {
		if (pstate[cpu]->local_tick != pstate[cpu]->cur_tick) {
			TRACE("BAD Quantum not acked on %d "
			      "(l:%lu c:%lu p:%lu)\n",
			      cpu,
			      pstate[cpu]->local_tick,
			      pstate[cpu]->cur_tick,
			      pfair_time);
			pstate[cpu]->missed_quanta++;
		}
		pstate[cpu]->cur_tick = time;
	}
	PTRACE(">>> Q %lu at %llu\n",
	       time, litmus_clock());
	raw_spin_unlock(&pfair_lock);
}

static noinline void wait_for_quantum(quanta_t q, struct pfair_state* state)
{
	quanta_t loc;

	goto first; /* skip mb() on first iteration */
	do {
		cpu_relax();
		mb();
	first:	loc = state->cur_tick;
		/* FIXME: what if loc > cur? */
	} while (time_before(loc, q));
	PTRACE("observed cur_tick:%lu >= q:%lu\n",
	       loc, q);
}

static quanta_t current_quantum(struct pfair_state* state)
{
	lt_t t = litmus_clock() - state->offset;
	return time2quanta(t, FLOOR);
}

static void catchup_quanta(quanta_t from, quanta_t target,
			   struct pfair_state* state)
{
	quanta_t cur = from, time;
	TRACE("+++< BAD catching up quanta from %lu to %lu\n",
	      from, target);
	while (time_before(cur, target)) {
		wait_for_quantum(cur, state);
		cur++;
		time = cmpxchg(&pfair_time,
			       cur - 1,   /* expected */
			       cur        /* next     */
			);
		if (time == cur - 1)
			schedule_next_quantum(cur);
	}
	TRACE("+++> catching up done\n");
}

/* pfair_tick - this function is called for every local timer
 *                         interrupt.
 */
static void pfair_tick(struct task_struct* t)
{
	struct pfair_state* state = &__get_cpu_var(pfair_state);
	quanta_t time, cur;
	int retry = 10;

	do {
		cur  = current_quantum(state);
		PTRACE("q %lu at %llu\n", cur, litmus_clock());

		/* Attempt to advance time. First CPU to get here
		 * will prepare the next quantum.
		 */
		time = cmpxchg(&pfair_time,
			       cur - 1,   /* expected */
			       cur        /* next     */
			);
		if (time == cur - 1) {
			/* exchange succeeded */
			wait_for_quantum(cur - 1, state);
			schedule_next_quantum(cur);
			retry = 0;
		} else if (time_before(time, cur - 1)) {
			/* the whole system missed a tick !? */
			catchup_quanta(time, cur, state);
			retry--;
		} else if (time_after(time, cur)) {
			/* our timer lagging behind!? */
			TRACE("BAD pfair_time:%lu > cur:%lu\n", time, cur);
			retry--;
		} else {
			/* Some other CPU already started scheduling
			 * this quantum. Let it do its job and then update.
			 */
			retry = 0;
		}
	} while (retry);

	/* Spin locally until time advances. */
	wait_for_quantum(cur, state);

	/* copy assignment */
	/* FIXME: what if we race with a future update? Corrupted state? */
	state->local      = state->linked;
	/* signal that we are done */
	mb();
	state->local_tick = state->cur_tick;

	if (state->local != current
	    && (is_realtime(current) || is_present(state->local)))
		litmus_reschedule_local();
}

static int safe_to_schedule(struct task_struct* t, int cpu)
{
	int where = tsk_rt(t)->scheduled_on;
	if (where != NO_CPU && where != cpu) {
		TRACE_TASK(t, "BAD: can't be scheduled on %d, "
			   "scheduled already on %d.\n", cpu, where);
		return 0;
	} else
		return tsk_rt(t)->present && get_rt_flags(t) == RT_F_RUNNING;
}

static struct task_struct* pfair_schedule(struct task_struct * prev)
{
	struct pfair_state* state = &__get_cpu_var(pfair_state);
	int blocks;
	struct task_struct* next = NULL;

	raw_spin_lock(&pfair_lock);

	blocks  = is_realtime(prev) && !is_running(prev);

	if (state->local && safe_to_schedule(state->local, state->cpu))
		next = state->local;

	if (prev != next) {
		tsk_rt(prev)->scheduled_on = NO_CPU;
		if (next)
			tsk_rt(next)->scheduled_on = state->cpu;
	}
	sched_state_task_picked();
	raw_spin_unlock(&pfair_lock);

	if (next)
		TRACE_TASK(next, "scheduled rel=%lu at %lu (%llu)\n",
			   tsk_pfair(next)->release, pfair_time, litmus_clock());
	else if (is_realtime(prev))
		TRACE("Becomes idle at %lu (%llu)\n", pfair_time, litmus_clock());

	return next;
}

static void pfair_task_new(struct task_struct * t, int on_rq, int running)
{
	unsigned long 		flags;

	TRACE("pfair: task new %d state:%d\n", t->pid, t->state);

	raw_spin_lock_irqsave(&pfair_lock, flags);
	if (running)
		t->rt_param.scheduled_on = task_cpu(t);
	else
		t->rt_param.scheduled_on = NO_CPU;

	prepare_release(t, pfair_time + 1);
	tsk_pfair(t)->sporadic_release = 0;
	pfair_add_release(t);
	check_preempt(t);

	raw_spin_unlock_irqrestore(&pfair_lock, flags);
}

static void pfair_task_wake_up(struct task_struct *t)
{
	unsigned long flags;
	lt_t now;

	TRACE_TASK(t, "wakes at %llu, release=%lu, pfair_time:%lu\n",
		   litmus_clock(), cur_release(t), pfair_time);

	raw_spin_lock_irqsave(&pfair_lock, flags);

	/* It is a little unclear how to deal with Pfair
	 * tasks that block for a while and then wake. For now,
	 * if a task blocks and wakes before its next job release,
	 * then it may resume if it is currently linked somewhere
	 * (as if it never blocked at all). Otherwise, we have a
	 * new sporadic job release.
	 */
	if (tsk_pfair(t)->sporadic_release) {
		now = litmus_clock();
		release_at(t, now);
		prepare_release(t, time2quanta(now, CEIL));
		sched_trace_task_release(t);
		/* FIXME: race with pfair_time advancing */
		pfair_add_release(t);
		tsk_pfair(t)->sporadic_release = 0;
	}

	check_preempt(t);

	raw_spin_unlock_irqrestore(&pfair_lock, flags);
	TRACE_TASK(t, "wake up done at %llu\n", litmus_clock());
}

static void pfair_task_block(struct task_struct *t)
{
	BUG_ON(!is_realtime(t));
	TRACE_TASK(t, "blocks at %llu, state:%d\n",
		   litmus_clock(), t->state);
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
	raw_spin_lock_irqsave(&pfair_lock, flags);

	TRACE_TASK(t, "RIP, state:%d\n", t->state);
	drop_all_references(t);

	raw_spin_unlock_irqrestore(&pfair_lock, flags);

	kfree(t->rt_param.pfair);
	t->rt_param.pfair = NULL;
}


static void pfair_release_at(struct task_struct* task, lt_t start)
{
	unsigned long flags;
	quanta_t release;

	BUG_ON(!is_realtime(task));

	raw_spin_lock_irqsave(&pfair_lock, flags);
	release_at(task, start);
	release = time2quanta(start, CEIL);

	if (release - pfair_time >= PFAIR_MAX_PERIOD)
		release = pfair_time + PFAIR_MAX_PERIOD;

	TRACE_TASK(task, "sys release at %lu\n", release);

	drop_all_references(task);
	prepare_release(task, release);
	pfair_add_release(task);

	/* Clear sporadic release flag, since this release subsumes any
	 * sporadic release on wake.
	 */
	tsk_pfair(task)->sporadic_release = 0;

	raw_spin_unlock_irqrestore(&pfair_lock, flags);
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
		if (period > quanta &&
		    do_div(tmp, (period - quanta))) /* ceil */
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

	if (quanta == period) {
		/* special case: task has weight 1.0 */
		printk(KERN_INFO
		       "Admitting weight 1.0 task. (%s/%d, %llu, %llu).\n",
		       t->comm, t->pid, quanta, period);
		quanta = 1;
		period = 1;
	}

	param = kmalloc(sizeof(*param) +
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

static long pfair_activate_plugin(void)
{
	int cpu;
	struct pfair_state* state;

	state = &__get_cpu_var(pfair_state);
	pfair_time = current_quantum(state);

	TRACE("Activating PFAIR at q=%lu\n", pfair_time);

	for (cpu = 0; cpu < num_online_cpus(); cpu++)  {
		state = &per_cpu(pfair_state, cpu);
		state->cur_tick   = pfair_time;
		state->local_tick = pfair_time;
		state->missed_quanta = 0;
		state->offset     = cpu_stagger_offset(cpu);
	}

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
	.complete_job		= complete_job,
	.activate_plugin	= pfair_activate_plugin,
};

static int __init init_pfair(void)
{
	int cpu, i;
	struct pfair_state *state;


	/*
	 * initialize short_cut for per-cpu pfair state;
	 * there may be a problem here if someone removes a cpu
	 * while we are doing this initialization... and if cpus
	 * are added / removed later... is it a _real_ problem?
	 */
	pstate = kmalloc(sizeof(struct pfair_state*) * num_online_cpus(), GFP_KERNEL);

	/* initialize release queue */
	for (i = 0; i < PFAIR_MAX_PERIOD; i++)
		bheap_init(&release_queue[i]);

	/* initialize CPU state */
	for (cpu = 0; cpu < num_online_cpus(); cpu++)  {
		state = &per_cpu(pfair_state, cpu);
		state->cpu 	  = cpu;
		state->cur_tick   = 0;
		state->local_tick = 0;
		state->linked     = NULL;
		state->local      = NULL;
		state->scheduled  = NULL;
		state->missed_quanta = 0;
		state->offset     = cpu_stagger_offset(cpu);
		pstate[cpu] = state;
	}

	rt_domain_init(&pfair, pfair_ready_order, NULL, NULL);
	return register_sched_plugin(&pfair_plugin);
}

static void __exit clean_pfair(void)
{
	kfree(pstate);
}

module_init(init_pfair);
module_exit(clean_pfair);
