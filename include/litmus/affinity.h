#ifndef __LITMUS_AFFINITY_H
#define __LITMUS_AFFINITY_H

#include <linux/cpumask.h>

/* Works like:
void get_nearest_available_cpu(
	cpu_entry_t **nearest,
	cpu_entry_t *start,
	cpu_entry_t *entries,
	int release_master,
	cpumask_var_t cpus_to_test)

Set release_master = NO_CPU for no Release Master.

We use a macro here to exploit the fact that C-EDF and G-EDF
have similar structures for their cpu_entry_t structs, even though
they do not share a common base-struct.  The macro allows us to
avoid code duplication.

 */
#define get_nearest_available_cpu(nearest, start, entries, release_master, cpus_to_test) \
{ \
	(nearest) = NULL; \
	if (!(start)->linked && likely((start)->cpu != (release_master))) { \
		(nearest) = (start); \
	} else { \
		int __cpu; \
		\
		/* FIXME: get rid of the iteration with a bitmask + AND */ \
		for_each_cpu(__cpu, cpus_to_test) { \
			if (likely(__cpu != release_master)) { \
				cpu_entry_t *__entry = &per_cpu((entries), __cpu); \
				if (cpus_share_cache((start)->cpu, __entry->cpu) \
				    && !__entry->linked) { \
					(nearest) = __entry; \
					break; \
				} \
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
