#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>
#include <linux/module.h>

#include <litmus/trace.h>

/******************************************************************************/
/*                          Allocation                                        */
/******************************************************************************/

struct ft_buffer* trace_ts_buf = NULL;

static unsigned int ts_seq_no = 0;

feather_callback void save_timestamp(unsigned long event)
{
	unsigned int seq_no = fetch_and_inc((int *) &ts_seq_no);
	struct timestamp *ts;
	if (ft_buffer_start_write(trace_ts_buf, (void**)  &ts)) {
		ts->event     = event;
		ts->timestamp = ft_read_tsc();
		ts->seq_no    = seq_no;
		ts->cpu       = raw_smp_processor_id();
		ft_buffer_finish_write(trace_ts_buf, ts);
	}
}

static struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size)
{
	struct ft_buffer* buf;
	size_t total = (size + 1) * count;
	char* mem;
	int order = 0, pages = 1;

	buf = kmalloc(sizeof(struct ft_buffer), GFP_KERNEL);
	if (!buf)
		return NULL;

	total = (total / PAGE_SIZE) + (total % PAGE_SIZE != 0);
	while (pages < total) {
		order++;
		pages *= 2;
	}

	mem = (char*) __get_free_pages(GFP_KERNEL, order);
	if (!mem) {
		kfree(buf);
		return NULL;
	}

	if (!init_ft_buffer(buf, count, size,
			    mem + (count * size),  /* markers at the end */
			    mem)) {                /* buffer objects     */
		free_pages((unsigned long) mem, order);
		kfree(buf);
		return NULL;
	}
	return buf;
}

static void free_ft_buffer(struct ft_buffer* buf)
{
	int order = 0, pages = 1;
	size_t total;

	if (buf) {
		total = (buf->slot_size + 1) * buf->slot_count;
		total = (total / PAGE_SIZE) + (total % PAGE_SIZE != 0);
		while (pages < total) {
			order++;
			pages *= 2;
		}
		free_pages((unsigned long) buf->buffer_mem, order);
		kfree(buf);
	}
}


/******************************************************************************/
/*                        DEVICE FILE DRIVER                                  */
/******************************************************************************/

#define NO_TIMESTAMPS 262144

static DECLARE_MUTEX(feather_lock);
static int use_count = 0;

static int trace_release(struct inode *in, struct file *filp)
{
	int err 		= -EINVAL;

	if (down_interruptible(&feather_lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	printk(KERN_ALERT "%s/%d disconnects from feather trace device. "
	       "use_count=%d\n",
	       current->comm, current->pid, use_count);

	if (use_count == 1) {
		/* disable events */
		ft_disable_all_events();

		/* wait for any pending events to complete */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);

		printk(KERN_ALERT "Failed trace writes: %u\n",
		       trace_ts_buf->failed_writes);

		free_ft_buffer(trace_ts_buf);
		trace_ts_buf = NULL;
	}

	use_count--;
	up(&feather_lock);
out:
	return err;
}


static ssize_t trace_read(struct file *filp, char __user *to, size_t len,
		      loff_t *f_pos)
{
	/* 	we ignore f_pos, this is strictly sequential */
	ssize_t error = 0;
	struct timestamp ts;

	if (down_interruptible(&feather_lock)) {
		error = -ERESTARTSYS;
		goto out;
	}


	while (len >= sizeof(struct timestamp)) {
		if (ft_buffer_read(trace_ts_buf, &ts)) {
			if (copy_to_user(to, &ts, sizeof(struct timestamp))) {
				error = -EFAULT;
				break;
			} else {
				len    -= sizeof(struct timestamp);
				to     += sizeof(struct timestamp);
				error  += sizeof(struct timestamp);
			}
	        } else {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(50);
			if (signal_pending(current)) {
				error = -ERESTARTSYS;
				break;
			}
		}
	}
	up(&feather_lock);
out:
	return error;
}

#define ENABLE_CMD 	0
#define DISABLE_CMD 	1

static ssize_t trace_write(struct file *filp, const char __user *from,
			   size_t len, loff_t *f_pos)
{
	ssize_t error = -EINVAL;
	unsigned long cmd;
	unsigned long id;

	if (len % sizeof(long) || len < 2 * sizeof(long))
		goto out;

	if (copy_from_user(&cmd, from, sizeof(long))) {
		error = -EFAULT;
	        goto out;
	}
	len  -= sizeof(long);
	from += sizeof(long);

	if (cmd != ENABLE_CMD && cmd != DISABLE_CMD)
		goto out;

	if (down_interruptible(&feather_lock)) {
		error = -ERESTARTSYS;
		goto out;
	}

	error = sizeof(long);
	while (len) {
		if (copy_from_user(&id, from, sizeof(long))) {
			error = -EFAULT;
			goto out;
		}
		len  -= sizeof(long);
		from += sizeof(long);
		if (cmd) {
			printk(KERN_INFO
			       "Disabling feather-trace event %lu.\n", id);
			ft_disable_event(id);
		} else {
			printk(KERN_INFO
			       "Enabling feather-trace event %lu.\n", id);
			ft_enable_event(id);
		}
		error += sizeof(long);
	}

	up(&feather_lock);
 out:
	return error;
}

static int trace_open(struct inode *in, struct file *filp)
{
	int err = 0;
        unsigned int count = NO_TIMESTAMPS;

	if (down_interruptible(&feather_lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	while (count && !trace_ts_buf) {
		printk("trace: trying to allocate %u time stamps.\n", count);
		trace_ts_buf = alloc_ft_buffer(count, sizeof(struct timestamp));
		count /= 2;
	}
	if (!trace_ts_buf)
		err = -ENOMEM;
	else
		use_count++;

	up(&feather_lock);
out:
	return err;
}

/******************************************************************************/
/*                          Device Registration                               */
/******************************************************************************/

#define FT_TRACE_MAJOR	252

struct file_operations ft_trace_fops = {
	.owner   = THIS_MODULE,
	.open    = trace_open,
	.release = trace_release,
	.write   = trace_write,
	.read    = trace_read,
};


static int __init register_buffer_dev(const char* name,
				      struct file_operations* fops,
				      int major, int count)
{
	dev_t   trace_dev;
	struct cdev *cdev;
	int error = 0;

	trace_dev = MKDEV(major, 0);
	error     = register_chrdev_region(trace_dev, count, name);
	if (error)
	{
		printk(KERN_WARNING "trace: "
		       "Could not register major/minor number %d\n", major);
		return error;
	}
	cdev = cdev_alloc();
	if (!cdev) {
		printk(KERN_WARNING "trace: "
			"Could not get a cdev for %s.\n", name);
		return -ENOMEM;
	}
	cdev->owner = THIS_MODULE;
	cdev->ops   = fops;
	error = cdev_add(cdev, trace_dev, count);
	if (error) {
		printk(KERN_WARNING "trace: "
			"add_cdev failed for %s.\n", name);
		return -ENOMEM;
	}
	return error;

}

static int __init init_sched_trace(void)
{
	int error = 0;

	printk("Initializing Feather-Trace device\n");
	/* dummy entry to make linker happy */
	ft_event0(666, save_timestamp);

	error = register_buffer_dev("ft_trace", &ft_trace_fops,
				    FT_TRACE_MAJOR, 1);
	return error;
}

module_init(init_sched_trace);
