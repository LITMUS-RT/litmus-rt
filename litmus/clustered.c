#include <linux/gfp.h>
#include <linux/cpumask.h>
#include <linux/list.h>

#include <litmus/clustered.h>

#if !defined(CONFIG_X86) || !defined(CONFIG_SYSFS)
/* fake get_shared_cpu_map() on non-x86 architectures */

int get_shared_cpu_map(cpumask_var_t mask, unsigned int cpu, int index)
{
	if (index != 1)
		return 1;
	else {
		/* Fake L1: CPU is all by itself. */
		cpumask_clear(mask);
		cpumask_set_cpu(cpu, mask);
		return 0;
	}
}

#endif

int get_cluster_size(enum cache_level level)
{
	cpumask_var_t mask;
	int ok;
	int num_cpus;

	if (level == GLOBAL_CLUSTER)
		return num_online_cpus();
	else {
		if (!zalloc_cpumask_var(&mask, GFP_ATOMIC))
			return -ENOMEM;
		/* assumes CPU 0 is representative of all CPUs */
		ok = get_shared_cpu_map(mask, 0, level);
		/* ok == 0 means we got the map; otherwise it's an invalid cache level */
		if (ok == 0)
			num_cpus = cpumask_weight(mask);
		free_cpumask_var(mask);

		if (ok == 0)
			return num_cpus;
		else
			return -EINVAL;
	}
}

int assign_cpus_to_clusters(enum cache_level level,
			    struct scheduling_cluster* clusters[],
			    unsigned int num_clusters,
			    struct cluster_cpu* cpus[],
			    unsigned int num_cpus)
{
	cpumask_var_t mask;
	unsigned int i, free_cluster = 0, low_cpu;
	int err = 0;

	if (!zalloc_cpumask_var(&mask, GFP_ATOMIC))
		return -ENOMEM;

	/* clear cluster pointers */
	for (i = 0; i < num_cpus; i++) {
		cpus[i]->id      = i;
		cpus[i]->cluster = NULL;
	}

	/* initialize clusters */
	for (i = 0; i < num_clusters; i++) {
		clusters[i]->id = i;
		INIT_LIST_HEAD(&clusters[i]->cpus);
	}

	/* Assign each CPU. Two assumtions are made:
	 * 1) The index of a cpu in cpus corresponds to its processor id (i.e., the index in a cpu mask).
	 * 2) All cpus that belong to some cluster are online.
	 */
	for_each_online_cpu(i) {
		/* get lowest-id CPU in cluster */
		if (level != GLOBAL_CLUSTER) {
			err = get_shared_cpu_map(mask, cpus[i]->id, level);
			if (err != 0) {
				/* ugh... wrong cache level? Either caller screwed up
				 * or the CPU topology is weird. */
				printk(KERN_ERR "Could not set up clusters for L%d sharing (max: L%d).\n",
				       level, err);
				err = -EINVAL;
				goto out;
			}
			low_cpu = cpumask_first(mask);
		} else
			low_cpu = 0;
		if (low_cpu == i) {
			/* caller must provide an appropriate number of clusters */
			BUG_ON(free_cluster >= num_clusters);

			/* create new cluster */
			cpus[i]->cluster = clusters[free_cluster++];
		} else {
			/* low_cpu points to the right cluster
			 * Assumption: low_cpu is actually online and was processed earlier. */
			cpus[i]->cluster = cpus[low_cpu]->cluster;
		}
		/* enqueue in cpus list */
		list_add_tail(&cpus[i]->cluster_list, &cpus[i]->cluster->cpus);
		printk(KERN_INFO "Assigning CPU%u to cluster %u\n.", i, cpus[i]->cluster->id);
	}
out:
	free_cpumask_var(mask);
	return err;
}
