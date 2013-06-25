/*
 * litmus_proc.c -- Implementation of the /proc/litmus directory tree.
 */

#include <linux/sched.h>
#include <linux/uaccess.h>

#include <litmus/litmus.h>
#include <litmus/litmus_proc.h>

#include <litmus/clustered.h>

/* in litmus/litmus.c */
extern atomic_t rt_task_count;

static struct proc_dir_entry *litmus_dir = NULL,
	*curr_file = NULL,
	*stat_file = NULL,
	*plugs_dir = NULL,
#ifdef CONFIG_RELEASE_MASTER
	*release_master_file = NULL,
#endif
	*plugs_file = NULL;

/* in litmus/sync.c */
int count_tasks_waiting_for_release(void);

static int proc_read_stats(char *page, char **start,
			   off_t off, int count,
			   int *eof, void *data)
{
	int len;

	len = snprintf(page, PAGE_SIZE,
		       "real-time tasks   = %d\n"
		       "ready for release = %d\n",
		       atomic_read(&rt_task_count),
		       count_tasks_waiting_for_release());
	return len;
}

static int proc_read_plugins(char *page, char **start,
			   off_t off, int count,
			   int *eof, void *data)
{
	int len;

	len = print_sched_plugins(page, PAGE_SIZE);
	return len;
}

static int proc_read_curr(char *page, char **start,
			  off_t off, int count,
			  int *eof, void *data)
{
	int len;

	len = snprintf(page, PAGE_SIZE, "%s\n", litmus->plugin_name);
	return len;
}

/* in litmus/litmus.c */
int switch_sched_plugin(struct sched_plugin*);

static int proc_write_curr(struct file *file,
			   const char *buffer,
			   unsigned long count,
			   void *data)
{
	int len, ret;
	char name[65];
	struct sched_plugin* found;

	len = copy_and_chomp(name, sizeof(name), buffer, count);
	if (len < 0)
		return len;

	found = find_sched_plugin(name);

	if (found) {
		ret = switch_sched_plugin(found);
		if (ret != 0)
			printk(KERN_INFO "Could not switch plugin: %d\n", ret);
	} else
		printk(KERN_INFO "Plugin '%s' is unknown.\n", name);

	return len;
}

#ifdef CONFIG_RELEASE_MASTER
static int proc_read_release_master(char *page, char **start,
				    off_t off, int count,
				    int *eof, void *data)
{
	int len, master;
	master = atomic_read(&release_master_cpu);
	if (master == NO_CPU)
		len = snprintf(page, PAGE_SIZE, "NO_CPU\n");
	else
		len = snprintf(page, PAGE_SIZE, "%d\n", master);
	return len;
}

static int proc_write_release_master(struct file *file,
				     const char *buffer,
				     unsigned long count,
				     void *data)
{
	int cpu, err, len, online = 0;
	char msg[64];

	len = copy_and_chomp(msg, sizeof(msg), buffer, count);

	if (len < 0)
		return len;

	if (strcmp(msg, "NO_CPU") == 0)
		atomic_set(&release_master_cpu, NO_CPU);
	else {
		err = sscanf(msg, "%d", &cpu);
		if (err == 1 && cpu >= 0 && (online = cpu_online(cpu))) {
			atomic_set(&release_master_cpu, cpu);
		} else {
			TRACE("invalid release master: '%s' "
			      "(err:%d cpu:%d online:%d)\n",
			      msg, err, cpu, online);
			len = -EINVAL;
		}
	}
	return len;
}
#endif

int __init init_litmus_proc(void)
{
	litmus_dir = proc_mkdir("litmus", NULL);
	if (!litmus_dir) {
		printk(KERN_ERR "Could not allocate LITMUS^RT procfs entry.\n");
		return -ENOMEM;
	}

	curr_file = create_proc_entry("active_plugin",
				      0644, litmus_dir);
	if (!curr_file) {
		printk(KERN_ERR "Could not allocate active_plugin "
		       "procfs entry.\n");
		return -ENOMEM;
	}
	curr_file->read_proc  = proc_read_curr;
	curr_file->write_proc = proc_write_curr;

#ifdef CONFIG_RELEASE_MASTER
	release_master_file = create_proc_entry("release_master",
						0644, litmus_dir);
	if (!release_master_file) {
		printk(KERN_ERR "Could not allocate release_master "
		       "procfs entry.\n");
		return -ENOMEM;
	}
	release_master_file->read_proc = proc_read_release_master;
	release_master_file->write_proc  = proc_write_release_master;
#endif

	stat_file = create_proc_read_entry("stats", 0444, litmus_dir,
					   proc_read_stats, NULL);

	plugs_dir = proc_mkdir("plugins", litmus_dir);
	if (!plugs_dir){
		printk(KERN_ERR "Could not allocate plugins directory "
				"procfs entry.\n");
		return -ENOMEM;
	}

	plugs_file = create_proc_read_entry("loaded", 0444, plugs_dir,
					   proc_read_plugins, NULL);

	return 0;
}

void exit_litmus_proc(void)
{
	if (plugs_file)
		remove_proc_entry("loaded", plugs_dir);
	if (plugs_dir)
		remove_proc_entry("plugins", litmus_dir);
	if (stat_file)
		remove_proc_entry("stats", litmus_dir);
	if (curr_file)
		remove_proc_entry("active_plugin", litmus_dir);
#ifdef CONFIG_RELEASE_MASTER
	if (release_master_file)
		remove_proc_entry("release_master", litmus_dir);
#endif
	if (litmus_dir)
		remove_proc_entry("litmus", NULL);
}

long make_plugin_proc_dir(struct sched_plugin* plugin,
		struct proc_dir_entry** pde_in)
{
	struct proc_dir_entry *pde_new = NULL;
	long rv;

	if (!plugin || !plugin->plugin_name){
		printk(KERN_ERR "Invalid plugin struct passed to %s.\n",
				__func__);
		rv = -EINVAL;
		goto out_no_pde;
	}

	if (!plugs_dir){
		printk(KERN_ERR "Could not make plugin sub-directory, because "
				"/proc/litmus/plugins does not exist.\n");
		rv = -ENOENT;
		goto out_no_pde;
	}

	pde_new = proc_mkdir(plugin->plugin_name, plugs_dir);
	if (!pde_new){
		printk(KERN_ERR "Could not make plugin sub-directory: "
				"out of memory?.\n");
		rv = -ENOMEM;
		goto out_no_pde;
	}

	rv = 0;
	*pde_in = pde_new;
	goto out_ok;

out_no_pde:
	*pde_in = NULL;
out_ok:
	return rv;
}

void remove_plugin_proc_dir(struct sched_plugin* plugin)
{
	if (!plugin || !plugin->plugin_name){
		printk(KERN_ERR "Invalid plugin struct passed to %s.\n",
				__func__);
		return;
	}
	remove_proc_entry(plugin->plugin_name, plugs_dir);
}



/* misc. I/O helper functions */

int copy_and_chomp(char *kbuf, unsigned long ksize,
		   __user const char* ubuf, unsigned long ulength)
{
	/* caller must provide buffer space */
	BUG_ON(!ksize);

	ksize--; /* leave space for null byte */

	if (ksize > ulength)
		ksize = ulength;

	if(copy_from_user(kbuf, ubuf, ksize))
		return -EFAULT;

	kbuf[ksize] = '\0';

	/* chomp kbuf */
	if (ksize > 0 && kbuf[ksize - 1] == '\n')
		kbuf[ksize - 1] = '\0';

	return ksize;
}

/* helper functions for clustered plugins */
static const char* cache_level_names[] = {
	"ALL",
	"L1",
	"L2",
	"L3",
};

int parse_cache_level(const char *cache_name, enum cache_level *level)
{
	int err = -EINVAL;
	int i;
	/* do a quick and dirty comparison to find the cluster size */
	for (i = GLOBAL_CLUSTER; i <= L3_CLUSTER; i++)
		if (!strcmp(cache_name, cache_level_names[i])) {
			*level = (enum cache_level) i;
			err = 0;
			break;
		}
	return err;
}

const char* cache_level_name(enum cache_level level)
{
	int idx = level;

	if (idx >= GLOBAL_CLUSTER && idx <= L3_CLUSTER)
		return cache_level_names[idx];
	else
		return "INVALID";
}


/* proc file interface to configure the cluster size */
static int proc_read_cluster_size(char *page, char **start,
				  off_t off, int count,
				  int *eof, void *data)
{
	return snprintf(page, PAGE_SIZE, "%s\n",
			cache_level_name(*((enum cache_level*) data)));;
}

static int proc_write_cluster_size(struct file *file,
				   const char *buffer,
				   unsigned long count,
				   void *data)
{
	int len;
	char cache_name[8];

	len = copy_and_chomp(cache_name, sizeof(cache_name), buffer, count);

	if (len > 0 && parse_cache_level(cache_name, (enum cache_level*) data))
		printk(KERN_INFO "Cluster '%s' is unknown.\n", cache_name);

	return len;
}

struct proc_dir_entry* create_cluster_file(struct proc_dir_entry* parent,
					   enum cache_level* level)
{
	struct proc_dir_entry* cluster_file;

	cluster_file = create_proc_entry("cluster", 0644, parent);
	if (!cluster_file) {
		printk(KERN_ERR "Could not allocate %s/cluster "
		       "procfs entry.\n", parent->name);
	} else {
		cluster_file->read_proc = proc_read_cluster_size;
		cluster_file->write_proc = proc_write_cluster_size;
		cluster_file->data = level;
	}
	return cluster_file;
}
