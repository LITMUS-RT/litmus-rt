#include <litmus/sched_plugin.h>
#include <linux/proc_fs.h>

int __init init_litmus_proc(void);
void exit_litmus_proc(void);

struct cd_mapping
{
	int id;
	cpumask_var_t mask;
	struct proc_dir_entry *proc_file;
};

struct domain_proc_info
{
	int num_cpus;
	int num_domains;

	struct cd_mapping *cpu_to_domains;
	struct cd_mapping *domain_to_cpus;
};

/*
 * On success, returns 0 and sets the pointer to the location of the new
 * proc dir entry, otherwise returns an error code and sets pde to NULL.
 */
long make_plugin_proc_dir(struct sched_plugin* plugin,
		struct proc_dir_entry** pde);

/*
 * Plugins should deallocate all child proc directory entries before
 * calling this, to avoid memory leaks.
 */
void remove_plugin_proc_dir(struct sched_plugin* plugin);

/*
 * Setup the CPU <-> sched domain mappings in proc
 */
long activate_domain_proc(struct domain_proc_info* map);

/*
 * Remove the CPU <-> sched domain mappings from proc
 */
long deactivate_domain_proc(void);

/*
 * Alloc memory for the mapping
 * Note: Does not set up proc files. Use make_sched_domain_maps for that.
 */
long init_domain_proc_info(struct domain_proc_info* map,
	int num_cpus, int num_domains);

/*
 * Free memory of the mapping
 * Note: Does not clean up proc files. Use deactivate_domain_proc for that.
 */
void destroy_domain_proc_info(struct domain_proc_info* map);

/* Copy at most size-1 bytes from ubuf into kbuf, null-terminate buf, and
 * remove a '\n' if present. Returns the number of bytes that were read or
 * -EFAULT. */
int copy_and_chomp(char *kbuf, unsigned long ksize,
		   __user const char* ubuf, unsigned long ulength);
