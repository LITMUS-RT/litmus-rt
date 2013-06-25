#include <linux/cpu.h>

#include <litmus/affinity.h>

struct neighborhood neigh_info[NR_CPUS];

/* called by _init_litmus() */
void init_topology(void) {
	int cpu;
	int i;
	int chk;
	int depth = num_cache_leaves;

	if (depth > NUM_CACHE_LEVELS)
		depth = NUM_CACHE_LEVELS;

	for_each_online_cpu(cpu) {
		for (i = 0; i < depth; ++i) {
			chk = get_shared_cpu_map((struct cpumask *)&neigh_info[cpu].neighbors[i], cpu, i);
			if (chk) {
				/* failed */
				neigh_info[cpu].size[i] = 0;
			} else {
				/* size = num bits in mask */
				neigh_info[cpu].size[i] =
					cpumask_weight((struct cpumask *)&neigh_info[cpu].neighbors[i]);
			}
			printk("CPU %d has %d neighbors at level %d. (mask = %lx)\n",
							cpu, neigh_info[cpu].size[i], i,
							*cpumask_bits(neigh_info[cpu].neighbors[i]));
		}

		/* set data for non-existent levels */
		for (; i < NUM_CACHE_LEVELS; ++i) {
			neigh_info[cpu].size[i] = 0;

			printk("CPU %d has %d neighbors at level %d. (mask = %lx)\n",
						cpu, neigh_info[cpu].size[i], i, 0lu);
		}
	}
}
