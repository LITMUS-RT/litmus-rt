#ifndef __LITMUS_AFFINITY_H
#define __LITMUS_AFFINITY_H

#include <linux/cpumask.h>

/*
  L1 (instr) = depth 0
  L1 (data)  = depth 1
  L2 = depth 2
  L3 = depth 3
 */
#define NUM_CACHE_LEVELS 4

struct neighborhood
{
	unsigned int size[NUM_CACHE_LEVELS];
	cpumask_var_t neighbors[NUM_CACHE_LEVELS];
};

/* topology info is stored redundently in a big array for fast lookups */
extern struct neighborhood neigh_info[NR_CPUS];

void init_topology(void); /* called by Litmus module's _init_litmus() */

/* Works like:
void get_nearest_available_cpu(
	cpu_entry_t **nearest,
	cpu_entry_t *start,
	cpu_entry_t *entries,
	int release_master)

Set release_master = NO_CPU for no Release Master.

We use a macro here to exploit the fact that C-EDF and G-EDF
have similar structures for their cpu_entry_t structs, even though
they do not share a common base-struct.  The macro allows us to
avoid code duplication.

TODO: Factor out the job-to-processor linking from C/G-EDF into
a reusable "processor mapping".  (See B.B.'s RTSS'09 paper &
dissertation.)
 */
#define get_nearest_available_cpu(nearest, start, entries, release_master) \
{ \
	(nearest) = NULL; \
	if (!(start)->linked) { \
		(nearest) = (start); \
	} else { \
		int __level; \
		int __cpu; \
		int __release_master = ((release_master) == NO_CPU) ? -1 : (release_master); \
		struct neighborhood *__neighbors = &neigh_info[(start)->cpu]; \
		\
		for (__level = 0; (__level < NUM_CACHE_LEVELS) && !(nearest); ++__level) { \
			if (__neighbors->size[__level] > 1) { \
				for_each_cpu(__cpu, __neighbors->neighbors[__level]) { \
					if (__cpu != __release_master) { \
						cpu_entry_t *__entry = &per_cpu((entries), __cpu); \
						if (!__entry->linked) { \
							(nearest) = __entry; \
							break; \
						} \
					} \
				} \
			} else if (__neighbors->size[__level] == 0) { \
				break; \
			} \
		} \
	} \
	\
	if ((nearest)) { \
		TRACE("P%d is closest available CPU to P%d\n", \
				(nearest)->cpu, (start)->cpu); \
	} else { \
		TRACE("Could not find an available CPU close to P%d\n", \
				(start)->cpu); \
	} \
}

#endif
