#ifndef CLUSTERED_H
#define CLUSTERED_H

/* Which cache level should be used to group CPUs into clusters?
 * GLOBAL_CLUSTER means that all CPUs form a single cluster (just like under
 * global scheduling).
 */
enum cache_level {
	GLOBAL_CLUSTER = 0,
	L1_CLUSTER     = 1,
	L2_CLUSTER     = 2,
	L3_CLUSTER     = 3
};

int parse_cache_level(const char *str, enum cache_level *level);
const char* cache_level_name(enum cache_level level);

/* expose a cache level in a /proc dir */
struct proc_dir_entry* create_cluster_file(struct proc_dir_entry* parent,
					   enum cache_level* level);



struct scheduling_cluster {
	unsigned int id;
	/* list of CPUs that are part of this cluster */
	struct list_head cpus;
};

struct cluster_cpu {
	unsigned int id; /* which CPU is this? */
	struct list_head cluster_list; /* List of the CPUs in this cluster. */
	struct scheduling_cluster* cluster; /* The cluster that this CPU belongs to. */
};

int get_cluster_size(enum cache_level level);

int assign_cpus_to_clusters(enum cache_level level,
			    struct scheduling_cluster* clusters[],
			    unsigned int num_clusters,
			    struct cluster_cpu* cpus[],
			    unsigned int num_cpus);

int get_shared_cpu_map(cpumask_var_t mask, unsigned int cpu, unsigned int index);

#endif
