/*
 * litmus_proc.c -- Implementation of the /proc/litmus directory tree.
 */

#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

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

static int litmus_stats_proc_show(struct seq_file *m, void *v)
{
        seq_printf(m,
		   "real-time tasks   = %d\n"
		   "ready for release = %d\n",
		   atomic_read(&rt_task_count),
		   count_tasks_waiting_for_release());
	return 0;
}

static int litmus_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, litmus_stats_proc_show, PDE_DATA(inode));
}

static const struct file_operations litmus_stats_proc_fops = {
	.open		= litmus_stats_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


static int litmus_loaded_proc_show(struct seq_file *m, void *v)
{
	print_sched_plugins(m);
	return 0;
}

static int litmus_loaded_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, litmus_loaded_proc_show, PDE_DATA(inode));
}

static const struct file_operations litmus_loaded_proc_fops = {
	.open		= litmus_loaded_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};




/* in litmus/litmus.c */
int switch_sched_plugin(struct sched_plugin*);

static ssize_t litmus_active_proc_write(struct file *file,
					const char __user *buffer, size_t count,
					loff_t *ppos)
{
	char name[65];
	struct sched_plugin* found;
	ssize_t ret = -EINVAL;
	int err;


	ret = copy_and_chomp(name, sizeof(name), buffer, count);
	if (ret < 0)
		return ret;

	found = find_sched_plugin(name);

	if (found) {
		err = switch_sched_plugin(found);
		if (err) {
			printk(KERN_INFO "Could not switch plugin: %d\n", err);
			ret = err;
		}
	} else {
		printk(KERN_INFO "Plugin '%s' is unknown.\n", name);
		ret = -ESRCH;
	}

	return ret;
}

static int litmus_active_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n", litmus->plugin_name);
	return 0;
}

static int litmus_active_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, litmus_active_proc_show, PDE_DATA(inode));
}

static const struct file_operations litmus_active_proc_fops = {
	.open		= litmus_active_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= litmus_active_proc_write,
};


#ifdef CONFIG_RELEASE_MASTER
static ssize_t litmus_release_master_proc_write(
	struct file *file,
	const char __user *buffer, size_t count,
	loff_t *ppos)
{
	int cpu, err, online = 0;
	char msg[64];
	ssize_t len;

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

static int litmus_release_master_proc_show(struct seq_file *m, void *v)
{
	int master;
	master = atomic_read(&release_master_cpu);
	if (master == NO_CPU)
		seq_printf(m, "NO_CPU\n");
	else
		seq_printf(m, "%d\n", master);
	return 0;
}

static int litmus_release_master_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, litmus_release_master_proc_show, PDE_DATA(inode));
}

static const struct file_operations litmus_release_master_proc_fops = {
	.open		= litmus_release_master_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= litmus_release_master_proc_write,
};
#endif

int __init init_litmus_proc(void)
{
	litmus_dir = proc_mkdir("litmus", NULL);
	if (!litmus_dir) {
		printk(KERN_ERR "Could not allocate LITMUS^RT procfs entry.\n");
		return -ENOMEM;
	}

	curr_file = proc_create("active_plugin", 0644, litmus_dir,
				&litmus_active_proc_fops);

	if (!curr_file) {
		printk(KERN_ERR "Could not allocate active_plugin "
		       "procfs entry.\n");
		return -ENOMEM;
	}

#ifdef CONFIG_RELEASE_MASTER
	release_master_file = proc_create("release_master", 0644, litmus_dir,
					  &litmus_release_master_proc_fops);
	if (!release_master_file) {
		printk(KERN_ERR "Could not allocate release_master "
		       "procfs entry.\n");
		return -ENOMEM;
	}
#endif

	stat_file = proc_create("stats", 0444, litmus_dir, &litmus_stats_proc_fops);

	plugs_dir = proc_mkdir("plugins", litmus_dir);
	if (!plugs_dir){
		printk(KERN_ERR "Could not allocate plugins directory "
				"procfs entry.\n");
		return -ENOMEM;
	}

	plugs_file = proc_create("loaded", 0444, plugs_dir,
				 &litmus_loaded_proc_fops);

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

static ssize_t litmus_cluster_proc_write(struct file *file,
					const char __user *buffer, size_t count,
					loff_t *ppos)
{
	enum cache_level *level = (enum cache_level *) PDE_DATA(file_inode(file));
	ssize_t len;
	char cache_name[8];

	len = copy_and_chomp(cache_name, sizeof(cache_name), buffer, count);

	if (len > 0 && parse_cache_level(cache_name, level)) {
		printk(KERN_INFO "Cluster '%s' is unknown.\n", cache_name);
		len = -EINVAL;
	}

	return len;
}

static int litmus_cluster_proc_show(struct seq_file *m, void *v)
{
	enum cache_level *level = (enum cache_level *)  m->private;

	seq_printf(m, "%s\n", cache_level_name(*level));
	return 0;
}

static int litmus_cluster_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, litmus_cluster_proc_show, PDE_DATA(inode));
}

static const struct file_operations litmus_cluster_proc_fops = {
	.open		= litmus_cluster_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= litmus_cluster_proc_write,
};

struct proc_dir_entry* create_cluster_file(struct proc_dir_entry* parent,
					   enum cache_level* level)
{
	struct proc_dir_entry* cluster_file;


	cluster_file = proc_create_data("cluster", 0644, parent,
					&litmus_cluster_proc_fops,
					(void *) level);
	if (!cluster_file) {
		printk(KERN_ERR
		       "Could not cluster procfs entry.\n");
	}
	return cluster_file;
}
