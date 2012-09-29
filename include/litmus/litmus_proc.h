#include <litmus/sched_plugin.h>
#include <linux/proc_fs.h>

int __init init_litmus_proc(void);
void exit_litmus_proc(void);

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


/* Copy at most size-1 bytes from ubuf into kbuf, null-terminate buf, and
 * remove a '\n' if present. Returns the number of bytes that were read or
 * -EFAULT. */
int copy_and_chomp(char *kbuf, unsigned long ksize,
		   __user const char* ubuf, unsigned long ulength);
