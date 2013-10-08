/*
 * kernel/sched_pfair.c
 *
 * Implementation of the PD^2 pfair scheduling algorithm. This
 * implementation realizes "early releasing," i.e., it is work-conserving.
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

/* to configure the cluster size */
#include <litmus/litmus_proc.h>

#include <litmus/clustered.h>

static enum cache_level pfair_cluster_level = GLOBAL_CLUSTER;

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

	struct pfair_cluster* cluster; /* where this task is scheduled */

	struct subtask subtasks[0];   /* allocate together with pfair_param */
};

#define tsk_pfair(tsk) ((tsk)->rt_param.pfair)

struct pfair_state {
	struct cluster_cpu topology;

	volatile quanta_t cur_tick;    /* updated by the CPU that is advancing
				        * the time */
	volatile quanta_t local_tick;  /* What tick is the local CPU currently
				        * executing? Updated only by the local
				        * CPU. In QEMU, this may lag behind the
				        * current tick. In a real system, with
				        * proper timers and aligned quanta,
				        * that should only be the case for a
				        * very short time after the time
				        * advanced. With staggered quanta, it
				        * will lag for the duration of the
				        * offset.
					*/

	struct task_struct* linked;    /* the task that should be executing */
	struct task_struct* local;     /* the local copy of linked          */
	struct task_struct* scheduled; /* what is actually scheduled        */

	lt_t offset;			/* stagger offset */
	unsigned int missed_updates;
	unsigned int missed_quanta;
};

struct pfair_cluster {
	struct scheduling_cluster topology;

	/* The "global" time in this cluster. */
	quanta_t pfair_time; /* the "official" PFAIR clock */

	/* The ready queue for this cluster. */
	rt_domain_t pfair;

	/* The set of jobs that should have their release enacted at the next
	 * quantum boundary.
	 */
	struct bheap release_queue;
	raw_spinlock_t release_lock;
};

#define FLAGS_NEED_REQUEUE 0x1

static inline struct pfair_cluster* cpu_cluster(struct pfair_state* state)
{
	return container_of(state->topology.cluster, struct pfair_cluster, topology);
}

static inline int cpu_id(struct pfair_state* state)
{
	return state->topology.id;
}

static inline struct pfair_state* from_cluster_list(struct list_head* pos)
{
	return list_entry(pos, struct pfair_state, topology.cluster_list);
}

static inline struct pfair_cluster* from_domain(rt_domain_t* rt)
{
	return container_of(rt, struct pfair_cluster, pfair);
}

static inline raw_spinlock_t* cluster_lock(struct pfair_cluster* cluster)
{
	/* The ready_lock is used to serialize all scheduling events. */
	return &cluster->pfair.ready_lock;
}

static inline raw_spinlock_t* cpu_lock(struct pfair_state* state)
{
	return cluster_lock(cpu_cluster(state));
}

DEFINE_PER_CPU(struct pfair_state, pfair_state);
struct pfair_state* *pstate; /* short cut */

static struct pfair_cluster* pfair_clusters;
static int num_pfair_clusters;

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

static quanta_t cur_release(struct task_struct* t)
{
	/* This is early releasing: only the release of the first subtask
	 * counts. */
	return tsk_pfair(t)->release;
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

static void pfair_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	struct pfair_cluster* cluster = from_domain(rt);
	unsigned long flags;

	raw_spin_lock_irqsave(&cluster->release_lock, flags);

	bheap_union(pfair_ready_order, &cluster->release_queue, tasks);

	raw_spin_unlock_irqrestore(&cluster->release_lock, flags);
}

static void prepare_release(struct task_struct* t, quanta_t at)
{
	tsk_pfair(t)->release    = at;
	tsk_pfair(t)->cur        = 0;
}

/* pull released tasks from the release queue */
static void poll_releases(struct pfair_cluster* cluster)
{
	raw_spin_lock(&cluster->release_lock);
	__merge_ready(&cluster->pfair, &cluster->release_queue);
	raw_spin_unlock(&cluster->release_lock);
}

static void check_preempt(struct task_struct* t)
{
	int cpu = NO_CPU;
	if (tsk_rt(t)->linked_on != tsk_rt(t)->scheduled_on &&
	    is_present(t)) {
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

/* caller must hold pfair.ready_lock */
static void drop_all_references(struct task_struct *t)
{
        int cpu;
        struct pfair_state* s;
	struct pfair_cluster* cluster;
        if (bheap_node_in_heap(tsk_rt(t)->heap_node)) {
                /* It must be in the ready queue; drop references isn't called
		 * when the job is in a release queue. */
		cluster = tsk_pfair(t)->cluster;
                bheap_delete(pfair_ready_order, &cluster->pfair.ready_queue,
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
	/* make sure we don't have a stale linked_on field */
	tsk_rt(t)->linked_on = NO_CPU;
}

static void pfair_prepare_next_period(struct task_struct* t)
{
	struct pfair_param* p = tsk_pfair(t);

	prepare_for_next_period(t);
	tsk_rt(t)->completed = 0;
	p->release = time2quanta(get_release(t), CEIL);
}

/* returns 1 if the task needs to go the release queue */
static int advance_subtask(quanta_t time, struct task_struct* t, int cpu)
{
	struct pfair_param* p = tsk_pfair(t);
	int to_relq;
	p->cur = (p->cur + 1) % p->quanta;
	if (!p->cur) {
		if (is_present(t)) {
			/* The job overran; we start a new budget allocation. */
			pfair_prepare_next_period(t);
		} else {
			/* remove task from system until it wakes */
			drop_all_references(t);
			tsk_rt(t)->flags |= FLAGS_NEED_REQUEUE;
			TRACE_TASK(t, "on %d advanced to subtask %lu (not present)\n",
				   cpu, p->cur);
			return 0;
		}
	}
	to_relq = time_after(cur_release(t), time);
	TRACE_TASK(t, "on %d advanced to subtask %lu -> to_relq=%d (cur_release:%lu time:%lu)\n",
		   cpu, p->cur, to_relq, cur_release(t), time);
	return to_relq;
}

static void advance_subtasks(struct pfair_cluster *cluster, quanta_t time)
{
	struct task_struct* l;
	struct pfair_param* p;
	struct list_head* pos;
	struct pfair_state* cpu;

	list_for_each(pos, &cluster->topology.cpus) {
		cpu = from_cluster_list(pos);
		l = cpu->linked;
		cpu->missed_updates += cpu->linked != cpu->local;
		if (l) {
			p = tsk_pfair(l);
			p->last_quantum = time;
			p->last_cpu     =  cpu_id(cpu);
			if (advance_subtask(time, l, cpu_id(cpu))) {
				//cpu->linked = NULL;
				PTRACE_TASK(l, "should go to release queue. "
					    "scheduled_on=%d present=%d\n",
					    tsk_rt(l)->scheduled_on,
					    tsk_rt(l)->present);
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
	struct pfair_cluster* cluster = cpu_cluster(pstate[cpu]);

	if (target != cpu) {
		BUG_ON(pstate[target]->topology.cluster != pstate[cpu]->topology.cluster);
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
				__add_ready(&cluster->pfair, prev);
			}
			/* we are done with this cpu */
			return 0;
		} else {
			/* re-add other, it's original CPU was not considered yet */
			tsk_rt(other)->linked_on = NO_CPU;
			__add_ready(&cluster->pfair, other);
			/* reschedule this CPU */
			return 1;
		}
	} else {
		pstate[cpu]->linked  = t;
		tsk_rt(t)->linked_on = cpu;
		if (prev) {
			/* prev got pushed back into the ready queue */
			tsk_rt(prev)->linked_on = NO_CPU;
			__add_ready(&cluster->pfair, prev);
		}
		/* we are done with this CPU */
		return 0;
	}
}

static void schedule_subtasks(struct pfair_cluster *cluster, quanta_t time)
{
	int retry;
	struct list_head *pos;
	struct pfair_state *cpu_state;

	list_for_each(pos, &cluster->topology.cpus) {
		cpu_state = from_cluster_list(pos);
		retry = 1;
#ifdef CONFIG_RELEASE_MASTER
		/* skip release master */
		if (cluster->pfair.release_master == cpu_id(cpu_state))
			continue;
#endif
		while (retry) {
			if (pfair_higher_prio(__peek_ready(&cluster->pfair),
					      cpu_state->linked))
				retry = pfair_link(time, cpu_id(cpu_state),
						   __take_ready(&cluster->pfair));
			else
				retry = 0;
		}
	}
}

static void schedule_next_quantum(struct pfair_cluster *cluster, quanta_t time)
{
	struct pfair_state *cpu;
	struct list_head* pos;

	/* called with interrupts disabled */
	PTRACE("--- Q %lu at %llu PRE-SPIN\n",
	       time, litmus_clock());
	raw_spin_lock(cluster_lock(cluster));
	PTRACE("<<< Q %lu at %llu\n",
	       time, litmus_clock());

	sched_trace_quantum_boundary();

	advance_subtasks(cluster, time);
	poll_releases(cluster);
	schedule_subtasks(cluster, time);

	list_for_each(pos, &cluster->topology.cpus) {
		cpu = from_cluster_list(pos);
		if (cpu->linked)
			PTRACE_TASK(cpu->linked,
				    " linked on %d.\n", cpu_id(cpu));
		else
			PTRACE("(null) linked on %d.\n", cpu_id(cpu));
	}
	/* We are done. Advance time. */
	mb();
	list_for_each(pos, &cluster->topology.cpus) {
		cpu = from_cluster_list(pos);
		if (cpu->local_tick != cpu->cur_tick) {
			TRACE("BAD Quantum not acked on %d "
			      "(l:%lu c:%lu p:%lu)\n",
			      cpu_id(cpu),
			      cpu->local_tick,
			      cpu->cur_tick,
			      cluster->pfair_time);
			cpu->missed_quanta++;
		}
		cpu->cur_tick = time;
	}
	PTRACE(">>> Q %lu at %llu\n",
	       time, litmus_clock());
	raw_spin_unlock(cluster_lock(cluster));
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
		time = cmpxchg(&cpu_cluster(state)->pfair_time,
			       cur - 1,   /* expected */
			       cur        /* next     */
			);
		if (time == cur - 1)
			schedule_next_quantum(cpu_cluster(state), cur);
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
		time = cmpxchg(&cpu_cluster(state)->pfair_time,
			       cur - 1,   /* expected */
			       cur        /* next     */
			);
		if (time == cur - 1) {
			/* exchange succeeded */
			wait_for_quantum(cur - 1, state);
			schedule_next_quantum(cpu_cluster(state), cur);
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
		return is_present(t) && !is_completed(t);
}

static struct task_struct* pfair_schedule(struct task_struct * prev)
{
	struct pfair_state* state = &__get_cpu_var(pfair_state);
	struct pfair_cluster* cluster = cpu_cluster(state);
	int blocks, completion, out_of_time;
	struct task_struct* next = NULL;

#ifdef CONFIG_RELEASE_MASTER
	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (unlikely(cluster->pfair.release_master == cpu_id(state))) {
		sched_state_task_picked();
		return NULL;
	}
#endif

	raw_spin_lock(cpu_lock(state));

	blocks      = is_realtime(prev) && !is_running(prev);
	completion  = is_realtime(prev) && is_completed(prev);
	out_of_time = is_realtime(prev) && time_after(cur_release(prev),
						      state->local_tick);

	if (is_realtime(prev))
	    PTRACE_TASK(prev, "blocks:%d completion:%d out_of_time:%d\n",
			blocks, completion, out_of_time);

	if (completion) {
		sched_trace_task_completion(prev, 0);
		pfair_prepare_next_period(prev);
		prepare_release(prev, cur_release(prev));
	}

	if (!blocks && (completion || out_of_time)) {
		drop_all_references(prev);
		sched_trace_task_release(prev);
		add_release(&cluster->pfair, prev);
	}

	if (state->local && safe_to_schedule(state->local, cpu_id(state)))
		next = state->local;

	if (prev != next) {
		tsk_rt(prev)->scheduled_on = NO_CPU;
		if (next)
			tsk_rt(next)->scheduled_on = cpu_id(state);
	}
	sched_state_task_picked();
	raw_spin_unlock(cpu_lock(state));

	if (next)
		TRACE_TASK(next, "scheduled rel=%lu at %lu (%llu)\n",
			   tsk_pfair(next)->release, cpu_cluster(state)->pfair_time, litmus_clock());
	else if (is_realtime(prev))
		TRACE("Becomes idle at %lu (%llu)\n", cpu_cluster(state)->pfair_time, litmus_clock());

	return next;
}

static void pfair_task_new(struct task_struct * t, int on_rq, int is_scheduled)
{
	unsigned long flags;
	struct pfair_cluster* cluster;

	TRACE("pfair: task new %d state:%d\n", t->pid, t->state);

	cluster = tsk_pfair(t)->cluster;

	raw_spin_lock_irqsave(cluster_lock(cluster), flags);

	prepare_release(t, cluster->pfair_time + 1);

	t->rt_param.scheduled_on = NO_CPU;
	t->rt_param.linked_on    = NO_CPU;

	if (is_scheduled) {
#ifdef CONFIG_RELEASE_MASTER
		if (task_cpu(t) != cluster->pfair.release_master)
#endif
			t->rt_param.scheduled_on = task_cpu(t);
	}

	if (is_running(t)) {
		tsk_rt(t)->present = 1;
		__add_ready(&cluster->pfair, t);
	} else {
		tsk_rt(t)->present = 0;
		tsk_rt(t)->flags |= FLAGS_NEED_REQUEUE;
	}

	check_preempt(t);

	raw_spin_unlock_irqrestore(cluster_lock(cluster), flags);
}

static void pfair_task_wake_up(struct task_struct *t)
{
	unsigned long flags;
	lt_t now;
	struct pfair_cluster* cluster;

	cluster = tsk_pfair(t)->cluster;

	TRACE_TASK(t, "wakes at %llu, release=%lu, pfair_time:%lu\n",
		   litmus_clock(), cur_release(t), cluster->pfair_time);

	raw_spin_lock_irqsave(cluster_lock(cluster), flags);

	/* If a task blocks and wakes before its next job release,
	 * then it may resume if it is currently linked somewhere
	 * (as if it never blocked at all). Otherwise, we have a
	 * new sporadic job release.
	 */
	now = litmus_clock();
	if (is_tardy(t, now)) {
		TRACE_TASK(t, "sporadic release!\n");
		release_at(t, now);
		prepare_release(t, time2quanta(now, CEIL));
		sched_trace_task_release(t);
	}

	/* only add to ready queue if the task isn't still linked somewhere */
	if (tsk_rt(t)->flags & FLAGS_NEED_REQUEUE) {
		tsk_rt(t)->flags &= ~FLAGS_NEED_REQUEUE;
		TRACE_TASK(t, "requeueing required\n");
		tsk_rt(t)->completed = 0;
		__add_ready(&cluster->pfair, t);
	}

	check_preempt(t);

	raw_spin_unlock_irqrestore(cluster_lock(cluster), flags);
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
	struct pfair_cluster *cluster;

	BUG_ON(!is_realtime(t));

	cluster = tsk_pfair(t)->cluster;

	/* Remote task from release or ready queue, and ensure
	 * that it is not the scheduled task for ANY CPU. We
	 * do this blanket check because occassionally when
	 * tasks exit while blocked, the task_cpu of the task
	 * might not be the same as the CPU that the PFAIR scheduler
	 * has chosen for it.
	 */
	raw_spin_lock_irqsave(cluster_lock(cluster), flags);

	TRACE_TASK(t, "RIP, state:%d\n", t->state);
	drop_all_references(t);

	raw_spin_unlock_irqrestore(cluster_lock(cluster), flags);

	kfree(t->rt_param.pfair);
	t->rt_param.pfair = NULL;
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

	/* first check that the task is in the right cluster */
	if (cpu_cluster(pstate[tsk_rt(t)->task_params.cpu]) !=
	    cpu_cluster(pstate[task_cpu(t)]))
		return -EINVAL;

	if (get_rt_period(t) != get_rt_relative_deadline(t)) {
		printk(KERN_INFO "%s: Admission rejected. "
			"Only implicit deadlines are currently supported.\n",
			litmus->plugin_name);
		return -EINVAL;
	}

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

	param->cluster = cpu_cluster(pstate[tsk_rt(t)->task_params.cpu]);

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

static void pfair_init_cluster(struct pfair_cluster* cluster)
{
	rt_domain_init(&cluster->pfair, pfair_ready_order, NULL, pfair_release_jobs);
	bheap_init(&cluster->release_queue);
	raw_spin_lock_init(&cluster->release_lock);
	INIT_LIST_HEAD(&cluster->topology.cpus);
}

static void cleanup_clusters(void)
{
	int i;

	if (num_pfair_clusters)
		kfree(pfair_clusters);
	pfair_clusters = NULL;
	num_pfair_clusters = 0;

	/* avoid stale pointers */
	for (i = 0; i < num_online_cpus(); i++) {
		pstate[i]->topology.cluster = NULL;
		printk("P%d missed %u updates and %u quanta.\n", cpu_id(pstate[i]),
		       pstate[i]->missed_updates, pstate[i]->missed_quanta);
	}
}

static long pfair_activate_plugin(void)
{
	int err, i;
	struct pfair_state* state;
	struct pfair_cluster* cluster ;
	quanta_t now;
	int cluster_size;
	struct cluster_cpu* cpus[NR_CPUS];
	struct scheduling_cluster* clust[NR_CPUS];

	cluster_size = get_cluster_size(pfair_cluster_level);

	if (cluster_size <= 0 || num_online_cpus() % cluster_size != 0)
		return -EINVAL;

	num_pfair_clusters = num_online_cpus() / cluster_size;

	pfair_clusters = kzalloc(num_pfair_clusters * sizeof(struct pfair_cluster), GFP_ATOMIC);
	if (!pfair_clusters) {
		num_pfair_clusters = 0;
		printk(KERN_ERR "Could not allocate Pfair clusters!\n");
		return -ENOMEM;
	}

	state = &__get_cpu_var(pfair_state);
	now = current_quantum(state);
	TRACE("Activating PFAIR at q=%lu\n", now);

	for (i = 0; i < num_pfair_clusters; i++) {
		cluster = &pfair_clusters[i];
		pfair_init_cluster(cluster);
		cluster->pfair_time = now;
		clust[i] = &cluster->topology;
#ifdef CONFIG_RELEASE_MASTER
		cluster->pfair.release_master = atomic_read(&release_master_cpu);
#endif
	}

	for (i = 0; i < num_online_cpus(); i++)  {
		state = &per_cpu(pfair_state, i);
		state->cur_tick   = now;
		state->local_tick = now;
		state->missed_quanta = 0;
		state->missed_updates = 0;
		state->offset     = cpu_stagger_offset(i);
		printk(KERN_ERR "cpus[%d] set; %d\n", i, num_online_cpus());
		cpus[i] = &state->topology;
	}

	err = assign_cpus_to_clusters(pfair_cluster_level, clust, num_pfair_clusters,
				      cpus, num_online_cpus());

	if (err < 0)
		cleanup_clusters();

	return err;
}

static long pfair_deactivate_plugin(void)
{
	cleanup_clusters();
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
	.complete_job		= complete_job,
	.activate_plugin	= pfair_activate_plugin,
	.deactivate_plugin	= pfair_deactivate_plugin,
};


static struct proc_dir_entry *cluster_file = NULL, *pfair_dir = NULL;

static int __init init_pfair(void)
{
	int cpu, err, fs;
	struct pfair_state *state;

	/*
	 * initialize short_cut for per-cpu pfair state;
	 * there may be a problem here if someone removes a cpu
	 * while we are doing this initialization... and if cpus
	 * are added / removed later... but we don't support CPU hotplug atm anyway.
	 */
	pstate = kmalloc(sizeof(struct pfair_state*) * num_online_cpus(), GFP_KERNEL);

	/* initialize CPU state */
	for (cpu = 0; cpu < num_online_cpus(); cpu++)  {
		state = &per_cpu(pfair_state, cpu);
		state->topology.id = cpu;
		state->cur_tick   = 0;
		state->local_tick = 0;
		state->linked     = NULL;
		state->local      = NULL;
		state->scheduled  = NULL;
		state->missed_quanta = 0;
		state->offset     = cpu_stagger_offset(cpu);
		pstate[cpu] = state;
	}

	pfair_clusters = NULL;
	num_pfair_clusters = 0;

	err = register_sched_plugin(&pfair_plugin);
	if (!err) {
		fs = make_plugin_proc_dir(&pfair_plugin, &pfair_dir);
		if (!fs)
			cluster_file = create_cluster_file(pfair_dir, &pfair_cluster_level);
		else
			printk(KERN_ERR "Could not allocate PFAIR procfs dir.\n");
	}

	return err;
}

static void __exit clean_pfair(void)
{
	kfree(pstate);

	if (cluster_file)
		remove_proc_entry("cluster", pfair_dir);
	if (pfair_dir)
		remove_plugin_proc_dir(&pfair_plugin);
}

module_init(init_pfair);
module_exit(clean_pfair);
