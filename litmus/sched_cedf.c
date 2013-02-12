/*
 * litmus/sched_cedf.c
 *
 * Implementation of the C-EDF scheduling algorithm.
 *
 * This implementation is based on G-EDF:
 * - CPUs are clustered around L2 or L3 caches.
 * - Clusters topology is automatically detected (this is arch dependent
 *   and is working only on x86 at the moment --- and only with modern
 *   cpus that exports cpuid4 information)
 * - The plugins _does not_ attempt to put tasks in the right cluster i.e.
 *   the programmer needs to be aware of the topology to place tasks
 *   in the desired cluster
 * - default clustering is around L2 cache (cache index = 2)
 *   supported clusters are: L1 (private cache: pedf), L2, L3, ALL (all
 *   online_cpus are placed in a single cluster).
 *
 *   For details on functions, take a look at sched_gsn_edf.c
 *
 * Currently, we do not support changes in the number of online cpus.
 * If the num_online_cpus() dynamically changes, the plugin is broken.
 *
 * This version uses the simple approach and serializes all scheduling
 * decisions by the use of a queue lock. This is probably not the
 * best way to do it, but it should suffice for now.
 */

#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/jobs.h>
#include <litmus/preempt.h>
#include <litmus/budget.h>
#include <litmus/sched_plugin.h>
#include <litmus/edf_common.h>
#include <litmus/sched_trace.h>

#include <litmus/clustered.h>

#include <litmus/bheap.h>

#ifdef CONFIG_SCHED_CPU_AFFINITY
#include <litmus/affinity.h>
#endif

/* to configure the cluster size */
#include <litmus/litmus_proc.h>
#include <linux/uaccess.h>

/* Reference configuration variable. Determines which cache level is used to
 * group CPUs into clusters.  GLOBAL_CLUSTER, which is the default, means that
 * all CPUs form a single cluster (just like GSN-EDF).
 */
static enum cache_level cluster_config = GLOBAL_CLUSTER;

struct clusterdomain;

/* cpu_entry_t - maintain the linked and scheduled state
 *
 * A cpu also contains a pointer to the cedf_domain_t cluster
 * that owns it (struct clusterdomain*)
 */
typedef struct  {
	int 			cpu;
	struct clusterdomain*	cluster;	/* owning cluster */
	struct task_struct*	linked;		/* only RT tasks */
	struct task_struct*	scheduled;	/* only RT tasks */
	atomic_t		will_schedule;	/* prevent unneeded IPIs */
	struct bheap_node*	hn;
} cpu_entry_t;

/* one cpu_entry_t per CPU */
DEFINE_PER_CPU(cpu_entry_t, cedf_cpu_entries);

#define set_will_schedule() \
	(atomic_set(&__get_cpu_var(cedf_cpu_entries).will_schedule, 1))
#define clear_will_schedule() \
	(atomic_set(&__get_cpu_var(cedf_cpu_entries).will_schedule, 0))
#define test_will_schedule(cpu) \
	(atomic_read(&per_cpu(cedf_cpu_entries, cpu).will_schedule))

/*
 * In C-EDF there is a cedf domain _per_ cluster
 * The number of clusters is dynamically determined accordingly to the
 * total cpu number and the cluster size
 */
typedef struct clusterdomain {
	/* rt_domain for this cluster */
	rt_domain_t	domain;
	/* cpus in this cluster */
	cpu_entry_t*	*cpus;
	/* map of this cluster cpus */
	cpumask_var_t	cpu_map;
	/* the cpus queue themselves according to priority in here */
	struct bheap_node *heap_node;
	struct bheap      cpu_heap;
	/* lock for this cluster */
#define cluster_lock domain.ready_lock
} cedf_domain_t;

/* a cedf_domain per cluster; allocation is done at init/activation time */
cedf_domain_t *cedf;

#define remote_cluster(cpu)	((cedf_domain_t *) per_cpu(cedf_cpu_entries, cpu).cluster)
#define task_cpu_cluster(task)	remote_cluster(get_partition(task))

/* Uncomment WANT_ALL_SCHED_EVENTS if you want to see all scheduling
 * decisions in the TRACE() log; uncomment VERBOSE_INIT for verbose
 * information during the initialization of the plugin (e.g., topology)
#define WANT_ALL_SCHED_EVENTS
 */
#define VERBOSE_INIT

static int cpu_lower_prio(struct bheap_node *_a, struct bheap_node *_b)
{
	cpu_entry_t *a, *b;
	a = _a->value;
	b = _b->value;
	/* Note that a and b are inverted: we want the lowest-priority CPU at
	 * the top of the heap.
	 */
	return edf_higher_prio(b->linked, a->linked);
}

/* update_cpu_position - Move the cpu entry to the correct place to maintain
 *                       order in the cpu queue. Caller must hold cedf lock.
 */
static void update_cpu_position(cpu_entry_t *entry)
{
	cedf_domain_t *cluster = entry->cluster;

	if (likely(bheap_node_in_heap(entry->hn)))
		bheap_delete(cpu_lower_prio,
				&cluster->cpu_heap,
				entry->hn);

	bheap_insert(cpu_lower_prio, &cluster->cpu_heap, entry->hn);
}

/* caller must hold cedf lock */
static cpu_entry_t* lowest_prio_cpu(cedf_domain_t *cluster)
{
	struct bheap_node* hn;
	hn = bheap_peek(cpu_lower_prio, &cluster->cpu_heap);
	return hn->value;
}


/* link_task_to_cpu - Update the link of a CPU.
 *                    Handles the case where the to-be-linked task is already
 *                    scheduled on a different CPU.
 */
static noinline void link_task_to_cpu(struct task_struct* linked,
				      cpu_entry_t *entry)
{
	cpu_entry_t *sched;
	struct task_struct* tmp;
	int on_cpu;

	BUG_ON(linked && !is_realtime(linked));

	/* Currently linked task is set to be unlinked. */
	if (entry->linked) {
		entry->linked->rt_param.linked_on = NO_CPU;
	}

	/* Link new task to CPU. */
	if (linked) {
		/* handle task is already scheduled somewhere! */
		on_cpu = linked->rt_param.scheduled_on;
		if (on_cpu != NO_CPU) {
			sched = &per_cpu(cedf_cpu_entries, on_cpu);
			/* this should only happen if not linked already */
			BUG_ON(sched->linked == linked);

			/* If we are already scheduled on the CPU to which we
			 * wanted to link, we don't need to do the swap --
			 * we just link ourselves to the CPU and depend on
			 * the caller to get things right.
			 */
			if (entry != sched) {
				TRACE_TASK(linked,
					   "already scheduled on %d, updating link.\n",
					   sched->cpu);
				tmp = sched->linked;
				linked->rt_param.linked_on = sched->cpu;
				sched->linked = linked;
				update_cpu_position(sched);
				linked = tmp;
			}
		}
		if (linked) /* might be NULL due to swap */
			linked->rt_param.linked_on = entry->cpu;
	}
	entry->linked = linked;
#ifdef WANT_ALL_SCHED_EVENTS
	if (linked)
		TRACE_TASK(linked, "linked to %d.\n", entry->cpu);
	else
		TRACE("NULL linked to %d.\n", entry->cpu);
#endif
	update_cpu_position(entry);
}

/* unlink - Make sure a task is not linked any longer to an entry
 *          where it was linked before. Must hold cedf_lock.
 */
static noinline void unlink(struct task_struct* t)
{
	cpu_entry_t *entry;

	if (t->rt_param.linked_on != NO_CPU) {
		/* unlink */
		entry = &per_cpu(cedf_cpu_entries, t->rt_param.linked_on);
		t->rt_param.linked_on = NO_CPU;
		link_task_to_cpu(NULL, entry);
	} else if (is_queued(t)) {
		/* This is an interesting situation: t is scheduled,
		 * but was just recently unlinked.  It cannot be
		 * linked anywhere else (because then it would have
		 * been relinked to this CPU), thus it must be in some
		 * queue. We must remove it from the list in this
		 * case.
		 *
		 * in C-EDF case is should be somewhere in the queue for
		 * its domain, therefore and we can get the domain using
		 * task_cpu_cluster
		 */
		remove(&(task_cpu_cluster(t))->domain, t);
	}
}


/* preempt - force a CPU to reschedule
 */
static void preempt(cpu_entry_t *entry)
{
	preempt_if_preemptable(entry->scheduled, entry->cpu);
}

/* requeue - Put an unlinked task into gsn-edf domain.
 *           Caller must hold cedf_lock.
 */
static noinline void requeue(struct task_struct* task)
{
	cedf_domain_t *cluster = task_cpu_cluster(task);
	BUG_ON(!task);
	/* sanity check before insertion */
	BUG_ON(is_queued(task));

	if (is_early_releasing(task) || is_released(task, litmus_clock()))
		__add_ready(&cluster->domain, task);
	else {
		/* it has got to wait */
		add_release(&cluster->domain, task);
	}
}

#ifdef CONFIG_SCHED_CPU_AFFINITY
static cpu_entry_t* cedf_get_nearest_available_cpu(
				cedf_domain_t *cluster, cpu_entry_t *start)
{
	cpu_entry_t *affinity;

	get_nearest_available_cpu(affinity, start, cedf_cpu_entries,
#ifdef CONFIG_RELEASE_MASTER
		cluster->domain.release_master
#else
		NO_CPU
#endif
		);

	/* make sure CPU is in our cluster */
	if (affinity && cpu_isset(affinity->cpu, *cluster->cpu_map))
		return(affinity);
	else
		return(NULL);
}
#endif


/* check for any necessary preemptions */
static void check_for_preemptions(cedf_domain_t *cluster)
{
	struct task_struct *task;
	cpu_entry_t *last;

#ifdef CONFIG_PREFER_LOCAL_LINKING
	cpu_entry_t *local;

	/* Before linking to other CPUs, check first whether the local CPU is
	 * idle. */
	local = &__get_cpu_var(cedf_cpu_entries);
	task  = __peek_ready(&cluster->domain);

	if (task && !local->linked
#ifdef CONFIG_RELEASE_MASTER
	    && likely(local->cpu != cluster->domain.release_master)
#endif
		) {
		task = __take_ready(&cluster->domain);
		TRACE_TASK(task, "linking to local CPU %d to avoid IPI\n", local->cpu);
		link_task_to_cpu(task, local);
		preempt(local);
	}
#endif


	for(last = lowest_prio_cpu(cluster);
	    edf_preemption_needed(&cluster->domain, last->linked);
	    last = lowest_prio_cpu(cluster)) {
		/* preemption necessary */
		task = __take_ready(&cluster->domain);
		TRACE("check_for_preemptions: attempting to link task %d to %d\n",
		      task->pid, last->cpu);
#ifdef CONFIG_SCHED_CPU_AFFINITY
		{
			cpu_entry_t *affinity =
					cedf_get_nearest_available_cpu(cluster,
						&per_cpu(cedf_cpu_entries, task_cpu(task)));
			if(affinity)
				last = affinity;
			else if(requeue_preempted_job(last->linked))
				requeue(last->linked);
		}
#else
		if (requeue_preempted_job(last->linked))
			requeue(last->linked);
#endif
		link_task_to_cpu(task, last);
		preempt(last);
	}
}

/* cedf_job_arrival: task is either resumed or released */
static noinline void cedf_job_arrival(struct task_struct* task)
{
	cedf_domain_t *cluster = task_cpu_cluster(task);
	BUG_ON(!task);

	requeue(task);
	check_for_preemptions(cluster);
}

static void cedf_release_jobs(rt_domain_t* rt, struct bheap* tasks)
{
	cedf_domain_t* cluster = container_of(rt, cedf_domain_t, domain);
	unsigned long flags;

	raw_spin_lock_irqsave(&cluster->cluster_lock, flags);

	__merge_ready(&cluster->domain, tasks);
	check_for_preemptions(cluster);

	raw_spin_unlock_irqrestore(&cluster->cluster_lock, flags);
}

/* caller holds cedf_lock */
static noinline void job_completion(struct task_struct *t, int forced)
{
	BUG_ON(!t);

	sched_trace_task_completion(t, forced);

	TRACE_TASK(t, "job_completion().\n");

	/* set flags */
	tsk_rt(t)->completed = 0;
	/* prepare for next period */
	prepare_for_next_period(t);
	if (is_early_releasing(t) || is_released(t, litmus_clock()))
		sched_trace_task_release(t);
	/* unlink */
	unlink(t);
	/* requeue
	 * But don't requeue a blocking task. */
	if (is_running(t))
		cedf_job_arrival(t);
}

/* Getting schedule() right is a bit tricky. schedule() may not make any
 * assumptions on the state of the current task since it may be called for a
 * number of reasons. The reasons include a scheduler_tick() determined that it
 * was necessary, because sys_exit_np() was called, because some Linux
 * subsystem determined so, or even (in the worst case) because there is a bug
 * hidden somewhere. Thus, we must take extreme care to determine what the
 * current state is.
 *
 * The CPU could currently be scheduling a task (or not), be linked (or not).
 *
 * The following assertions for the scheduled task could hold:
 *
 *      - !is_running(scheduled)        // the job blocks
 *	- scheduled->timeslice == 0	// the job completed (forcefully)
 *	- is_completed()		// the job completed (by syscall)
 * 	- linked != scheduled		// we need to reschedule (for any reason)
 * 	- is_np(scheduled)		// rescheduling must be delayed,
 *					   sys_exit_np must be requested
 *
 * Any of these can occur together.
 */
static struct task_struct* cedf_schedule(struct task_struct * prev)
{
	cpu_entry_t* entry = &__get_cpu_var(cedf_cpu_entries);
	cedf_domain_t *cluster = entry->cluster;
	int out_of_time, sleep, preempt, np, exists, blocks;
	struct task_struct* next = NULL;

#ifdef CONFIG_RELEASE_MASTER
	/* Bail out early if we are the release master.
	 * The release master never schedules any real-time tasks.
	 */
	if (unlikely(cluster->domain.release_master == entry->cpu)) {
		sched_state_task_picked();
		return NULL;
	}
#endif

	raw_spin_lock(&cluster->cluster_lock);
	clear_will_schedule();

	/* sanity checking */
	BUG_ON(entry->scheduled && entry->scheduled != prev);
	BUG_ON(entry->scheduled && !is_realtime(prev));
	BUG_ON(is_realtime(prev) && !entry->scheduled);

	/* (0) Determine state */
	exists      = entry->scheduled != NULL;
	blocks      = exists && !is_running(entry->scheduled);
	out_of_time = exists &&
				  budget_enforced(entry->scheduled) &&
				  budget_exhausted(entry->scheduled);
	np 	    = exists && is_np(entry->scheduled);
	sleep	    = exists && is_completed(entry->scheduled);
	preempt     = entry->scheduled != entry->linked;

#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "invoked cedf_schedule.\n");
#endif

	if (exists)
		TRACE_TASK(prev,
			   "blocks:%d out_of_time:%d np:%d sleep:%d preempt:%d "
			   "state:%d sig:%d\n",
			   blocks, out_of_time, np, sleep, preempt,
			   prev->state, signal_pending(prev));
	if (entry->linked && preempt)
		TRACE_TASK(prev, "will be preempted by %s/%d\n",
			   entry->linked->comm, entry->linked->pid);


	/* If a task blocks we have no choice but to reschedule.
	 */
	if (blocks)
		unlink(entry->scheduled);

	/* Request a sys_exit_np() call if we would like to preempt but cannot.
	 * We need to make sure to update the link structure anyway in case
	 * that we are still linked. Multiple calls to request_exit_np() don't
	 * hurt.
	 */
	if (np && (out_of_time || preempt || sleep)) {
		unlink(entry->scheduled);
		request_exit_np(entry->scheduled);
	}

	/* Any task that is preemptable and either exhausts its execution
	 * budget or wants to sleep completes. We may have to reschedule after
	 * this. Don't do a job completion if we block (can't have timers running
	 * for blocked jobs).
	 */
	if (!np && (out_of_time || sleep) && !blocks)
		job_completion(entry->scheduled, !sleep);

	/* Link pending task if we became unlinked.
	 */
	if (!entry->linked)
		link_task_to_cpu(__take_ready(&cluster->domain), entry);

	/* The final scheduling decision. Do we need to switch for some reason?
	 * If linked is different from scheduled, then select linked as next.
	 */
	if ((!np || blocks) &&
	    entry->linked != entry->scheduled) {
		/* Schedule a linked job? */
		if (entry->linked) {
			entry->linked->rt_param.scheduled_on = entry->cpu;
			next = entry->linked;
		}
		if (entry->scheduled) {
			/* not gonna be scheduled soon */
			entry->scheduled->rt_param.scheduled_on = NO_CPU;
			TRACE_TASK(entry->scheduled, "scheduled_on = NO_CPU\n");
		}
	} else
		/* Only override Linux scheduler if we have a real-time task
		 * scheduled that needs to continue.
		 */
		if (exists)
			next = prev;

	sched_state_task_picked();
	raw_spin_unlock(&cluster->cluster_lock);

#ifdef WANT_ALL_SCHED_EVENTS
	TRACE("cedf_lock released, next=0x%p\n", next);

	if (next)
		TRACE_TASK(next, "scheduled at %llu\n", litmus_clock());
	else if (exists && !next)
		TRACE("becomes idle at %llu.\n", litmus_clock());
#endif


	return next;
}


/* _finish_switch - we just finished the switch away from prev
 */
static void cedf_finish_switch(struct task_struct *prev)
{
	cpu_entry_t* 	entry = &__get_cpu_var(cedf_cpu_entries);

	entry->scheduled = is_realtime(current) ? current : NULL;
#ifdef WANT_ALL_SCHED_EVENTS
	TRACE_TASK(prev, "switched away from\n");
#endif
}


/*	Prepare a task for running in RT mode
 */
static void cedf_task_new(struct task_struct * t, int on_rq, int is_scheduled)
{
	unsigned long 		flags;
	cpu_entry_t* 		entry;
	cedf_domain_t*		cluster;

	TRACE("gsn edf: task new %d\n", t->pid);

	/* the cluster doesn't change even if t is scheduled */
	cluster = task_cpu_cluster(t);

	raw_spin_lock_irqsave(&cluster->cluster_lock, flags);

	/* setup job params */
	release_at(t, litmus_clock());

	if (is_scheduled) {
		entry = &per_cpu(cedf_cpu_entries, task_cpu(t));
		BUG_ON(entry->scheduled);

#ifdef CONFIG_RELEASE_MASTER
		if (entry->cpu != cluster->domain.release_master) {
#endif
			entry->scheduled = t;
			tsk_rt(t)->scheduled_on = task_cpu(t);
#ifdef CONFIG_RELEASE_MASTER
		} else {
			/* do not schedule on release master */
			preempt(entry); /* force resched */
			tsk_rt(t)->scheduled_on = NO_CPU;
		}
#endif
	} else {
		t->rt_param.scheduled_on = NO_CPU;
	}
	t->rt_param.linked_on          = NO_CPU;

	if (is_running(t))
		cedf_job_arrival(t);
	raw_spin_unlock_irqrestore(&(cluster->cluster_lock), flags);
}

static void cedf_task_wake_up(struct task_struct *task)
{
	unsigned long flags;
	lt_t now;
	cedf_domain_t *cluster;

	TRACE_TASK(task, "wake_up at %llu\n", litmus_clock());

	cluster = task_cpu_cluster(task);

	raw_spin_lock_irqsave(&cluster->cluster_lock, flags);
	now = litmus_clock();
	if (is_sporadic(task) && is_tardy(task, now)) {
		/* new sporadic release */
		release_at(task, now);
		sched_trace_task_release(task);
	}
	cedf_job_arrival(task);
	raw_spin_unlock_irqrestore(&cluster->cluster_lock, flags);
}

static void cedf_task_block(struct task_struct *t)
{
	unsigned long flags;
	cedf_domain_t *cluster;

	TRACE_TASK(t, "block at %llu\n", litmus_clock());

	cluster = task_cpu_cluster(t);

	/* unlink if necessary */
	raw_spin_lock_irqsave(&cluster->cluster_lock, flags);
	unlink(t);
	raw_spin_unlock_irqrestore(&cluster->cluster_lock, flags);

	BUG_ON(!is_realtime(t));
}


static void cedf_task_exit(struct task_struct * t)
{
	unsigned long flags;
	cedf_domain_t *cluster = task_cpu_cluster(t);

	/* unlink if necessary */
	raw_spin_lock_irqsave(&cluster->cluster_lock, flags);
	unlink(t);
	if (tsk_rt(t)->scheduled_on != NO_CPU) {
		cpu_entry_t *cpu;
		cpu = &per_cpu(cedf_cpu_entries, tsk_rt(t)->scheduled_on);
		cpu->scheduled = NULL;
		tsk_rt(t)->scheduled_on = NO_CPU;
	}
	raw_spin_unlock_irqrestore(&cluster->cluster_lock, flags);

	BUG_ON(!is_realtime(t));
        TRACE_TASK(t, "RIP\n");
}

static long cedf_admit_task(struct task_struct* tsk)
{
	return (remote_cluster(task_cpu(tsk)) == task_cpu_cluster(tsk)) ?
			0 : -EINVAL;
}

/* total number of cluster */
static int num_clusters;
/* we do not support cluster of different sizes */
static unsigned int cluster_size;

#ifdef VERBOSE_INIT
static void print_cluster_topology(cpumask_var_t mask, int cpu)
{
	int chk;
	char buf[255];

	chk = cpulist_scnprintf(buf, 254, mask);
	buf[chk] = '\0';
	printk(KERN_INFO "CPU = %d, shared cpu(s) = %s\n", cpu, buf);

}
#endif

static int clusters_allocated = 0;

static void cleanup_cedf(void)
{
	int i;

	if (clusters_allocated) {
		for (i = 0; i < num_clusters; i++) {
			kfree(cedf[i].cpus);
			kfree(cedf[i].heap_node);
			free_cpumask_var(cedf[i].cpu_map);
		}

		kfree(cedf);
	}
}

static struct domain_proc_info cedf_domain_proc_info;
static long cedf_get_domain_proc_info(struct domain_proc_info **ret)
{
	*ret = &cedf_domain_proc_info;
	return 0;
}

static void cedf_setup_domain_proc(void)
{
	int i, cpu, domain;
#ifdef CONFIG_RELEASE_MASTER
	int release_master = atomic_read(&release_master_cpu);
	/* skip over the domain with the release master if cluster size is 1 */
	int skip_domain = (1 == cluster_size && release_master != NO_CPU) ?
			release_master : NO_CPU;
#else
	int release_master = NO_CPU;
	int skip_domain = NO_CPU;
#endif
	int num_rt_cpus = num_online_cpus() - (release_master != NO_CPU);
	int num_rt_domains = num_clusters - (skip_domain != NO_CPU);
	struct cd_mapping *map;

	memset(&cedf_domain_proc_info, sizeof(cedf_domain_proc_info), 0);
	init_domain_proc_info(&cedf_domain_proc_info, num_rt_cpus, num_rt_domains);
	cedf_domain_proc_info.num_cpus = num_rt_cpus;
	cedf_domain_proc_info.num_domains = num_rt_domains;

	for (cpu = 0, i = 0; cpu < num_online_cpus(); ++cpu) {
		if (cpu == release_master)
			continue;
		map = &cedf_domain_proc_info.cpu_to_domains[i];
		/* pointer math to figure out the domain index */
		domain = remote_cluster(cpu) - cedf;
		map->id = cpu;
		cpumask_set_cpu(domain, map->mask);
		++i;
	}

	for (domain = 0, i = 0; domain < num_clusters; ++domain) {
		if (domain == skip_domain)
			continue;
		map = &cedf_domain_proc_info.domain_to_cpus[i];
		map->id = i;
		cpumask_copy(map->mask, cedf[domain].cpu_map);
		++i;
	}
}

static long cedf_activate_plugin(void)
{
	int i, j, cpu, ccpu, cpu_count;
	cpu_entry_t *entry;

	cpumask_var_t mask;
	int chk = 0;

	/* de-allocate old clusters, if any */
	cleanup_cedf();

	printk(KERN_INFO "C-EDF: Activate Plugin, cluster configuration = %d\n",
			cluster_config);

	/* need to get cluster_size first */
	if(!zalloc_cpumask_var(&mask, GFP_ATOMIC))
		return -ENOMEM;

	if (cluster_config == GLOBAL_CLUSTER) {
		cluster_size = num_online_cpus();
	} else {
		chk = get_shared_cpu_map(mask, 0, cluster_config);
		if (chk) {
			/* if chk != 0 then it is the max allowed index */
			printk(KERN_INFO "C-EDF: Cluster configuration = %d "
			       "is not supported on this hardware.\n",
			       cluster_config);
			/* User should notice that the configuration failed, so
			 * let's bail out. */
			return -EINVAL;
		}

		cluster_size = cpumask_weight(mask);
	}

	if ((num_online_cpus() % cluster_size) != 0) {
		/* this can't be right, some cpus are left out */
		printk(KERN_ERR "C-EDF: Trying to group %d cpus in %d!\n",
				num_online_cpus(), cluster_size);
		return -1;
	}

	num_clusters = num_online_cpus() / cluster_size;
	printk(KERN_INFO "C-EDF: %d cluster(s) of size = %d\n",
			num_clusters, cluster_size);

	/* initialize clusters */
	cedf = kmalloc(num_clusters * sizeof(cedf_domain_t), GFP_ATOMIC);
	for (i = 0; i < num_clusters; i++) {

		cedf[i].cpus = kmalloc(cluster_size * sizeof(cpu_entry_t),
				GFP_ATOMIC);
		cedf[i].heap_node = kmalloc(
				cluster_size * sizeof(struct bheap_node),
				GFP_ATOMIC);
		bheap_init(&(cedf[i].cpu_heap));
		edf_domain_init(&(cedf[i].domain), NULL, cedf_release_jobs);

		if(!zalloc_cpumask_var(&cedf[i].cpu_map, GFP_ATOMIC))
			return -ENOMEM;
#ifdef CONFIG_RELEASE_MASTER
		cedf[i].domain.release_master = atomic_read(&release_master_cpu);
#endif
	}

	/* cycle through cluster and add cpus to them */
	for (i = 0; i < num_clusters; i++) {

		for_each_online_cpu(cpu) {
			/* check if the cpu is already in a cluster */
			for (j = 0; j < num_clusters; j++)
				if (cpumask_test_cpu(cpu, cedf[j].cpu_map))
					break;
			/* if it is in a cluster go to next cpu */
			if (j < num_clusters &&
					cpumask_test_cpu(cpu, cedf[j].cpu_map))
				continue;

			/* this cpu isn't in any cluster */
			/* get the shared cpus */
			if (unlikely(cluster_config == GLOBAL_CLUSTER))
				cpumask_copy(mask, cpu_online_mask);
			else
				get_shared_cpu_map(mask, cpu, cluster_config);

			cpumask_copy(cedf[i].cpu_map, mask);
#ifdef VERBOSE_INIT
			print_cluster_topology(mask, cpu);
#endif
			/* add cpus to current cluster and init cpu_entry_t */
			cpu_count = 0;
			for_each_cpu(ccpu, cedf[i].cpu_map) {

				entry = &per_cpu(cedf_cpu_entries, ccpu);
				cedf[i].cpus[cpu_count] = entry;
				atomic_set(&entry->will_schedule, 0);
				entry->cpu = ccpu;
				entry->cluster = &cedf[i];
				entry->hn = &(cedf[i].heap_node[cpu_count]);
				bheap_node_init(&entry->hn, entry);

				cpu_count++;

				entry->linked = NULL;
				entry->scheduled = NULL;
#ifdef CONFIG_RELEASE_MASTER
				/* only add CPUs that should schedule jobs */
				if (entry->cpu != entry->cluster->domain.release_master)
#endif
					update_cpu_position(entry);
			}
			/* done with this cluster */
			break;
		}
	}

	clusters_allocated = 1;
	free_cpumask_var(mask);

	cedf_setup_domain_proc();

	return 0;
}

static long cedf_deactivate_plugin(void)
{
	destroy_domain_proc_info(&cedf_domain_proc_info);
	return 0;
}

/*	Plugin object	*/
static struct sched_plugin cedf_plugin __cacheline_aligned_in_smp = {
	.plugin_name		= "C-EDF",
	.finish_switch		= cedf_finish_switch,
	.task_new		= cedf_task_new,
	.complete_job		= complete_job,
	.task_exit		= cedf_task_exit,
	.schedule		= cedf_schedule,
	.task_wake_up		= cedf_task_wake_up,
	.task_block		= cedf_task_block,
	.admit_task		= cedf_admit_task,
	.activate_plugin	= cedf_activate_plugin,
	.deactivate_plugin	= cedf_deactivate_plugin,
	.get_domain_proc_info	= cedf_get_domain_proc_info,
};

static struct proc_dir_entry *cluster_file = NULL, *cedf_dir = NULL;

static int __init init_cedf(void)
{
	int err, fs;

	err = register_sched_plugin(&cedf_plugin);
	if (!err) {
		fs = make_plugin_proc_dir(&cedf_plugin, &cedf_dir);
		if (!fs)
			cluster_file = create_cluster_file(cedf_dir, &cluster_config);
		else
			printk(KERN_ERR "Could not allocate C-EDF procfs dir.\n");
	}
	return err;
}

static void clean_cedf(void)
{
	cleanup_cedf();
	if (cluster_file)
		remove_proc_entry("cluster", cedf_dir);
	if (cedf_dir)
		remove_plugin_proc_dir(&cedf_plugin);
}

module_init(init_cedf);
module_exit(clean_cedf);
