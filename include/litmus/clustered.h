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

#endif
