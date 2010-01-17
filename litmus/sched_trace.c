/*
 * sched_trace.c -- record scheduling events to a byte stream.
 */
#include <linux/spinlock.h>
#include <linux/semaphore.h>

#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/sysrq.h>

#include <linux/kfifo.h>

#include <litmus/sched_trace.h>
#include <litmus/litmus.h>

/* Allocate a buffer of about 32k per CPU */
#define LITMUS_TRACE_BUF_PAGES 8
#define LITMUS_TRACE_BUF_SIZE (PAGE_SIZE * LITMUS_TRACE_BUF_PAGES * NR_CPUS)

/* Max length of one read from the buffer */
#define MAX_READ_LEN (64 * 1024)

/* Max length for one write --- from kernel --- to the buffer */
#define MSG_SIZE 255

/*
 * Major number for the tracing char device.
 * the major numbes are from the unassigned/local use block
 *
 * set MAJOR to 0 to have it dynamically assigned
 */
#define LOG_MAJOR	251

/* Inner ring buffer structure */
typedef struct {
	rwlock_t	del_lock;

	/* the buffer */
	struct kfifo	*kfifo;
} ring_buffer_t;

/* Main buffer structure */
typedef struct {
	ring_buffer_t 		buf;
	atomic_t		reader_cnt;
	struct semaphore	reader_mutex;
} trace_buffer_t;


/*
 * Inner buffer management functions
 */
void rb_init(ring_buffer_t* buf)
{
	rwlock_init(&buf->del_lock);
	buf->kfifo = NULL;
}

int rb_alloc_buf(ring_buffer_t* buf, unsigned int size)
{
	unsigned long flags;

	write_lock_irqsave(&buf->del_lock, flags);

	buf->kfifo = kfifo_alloc(size, GFP_ATOMIC, NULL);

	write_unlock_irqrestore(&buf->del_lock, flags);

	if(IS_ERR(buf->kfifo)) {
		printk(KERN_ERR "kfifo_alloc failed\n");
		return PTR_ERR(buf->kfifo);
	}

	return 0;
}

int rb_free_buf(ring_buffer_t* buf)
{
	unsigned long flags;

	write_lock_irqsave(&buf->del_lock, flags);

	BUG_ON(!buf->kfifo);
	kfifo_free(buf->kfifo);

	buf->kfifo = NULL;

	write_unlock_irqrestore(&buf->del_lock, flags);

	return 0;
}

/*
 * Assumption: concurrent writes are serialized externally
 *
 * Will only succeed if there is enough space for all len bytes.
 */
int rb_put(ring_buffer_t* buf, char* mem, size_t len)
{
	unsigned long flags;
	int error = 0;

	read_lock_irqsave(&buf->del_lock, flags);

	if (!buf->kfifo) {
		error = -ENODEV;
		goto out;
	}

	if((__kfifo_put(buf->kfifo, mem, len)) < len) {
		error = -ENOMEM;
		goto out;
	}

 out:
	read_unlock_irqrestore(&buf->del_lock, flags);
	return error;
}

/* Assumption: concurrent reads are serialized externally */
int rb_get(ring_buffer_t* buf, char* mem, size_t len)
{
	unsigned long flags;
	int error = 0;

	read_lock_irqsave(&buf->del_lock, flags);
	if (!buf->kfifo) {
		error = -ENODEV;
		goto out;
	}

	error = __kfifo_get(buf->kfifo, (unsigned char*)mem, len);

 out:
	read_unlock_irqrestore(&buf->del_lock, flags);
	return error;
}

/*
 * Device Driver management
 */
static spinlock_t log_buffer_lock = SPIN_LOCK_UNLOCKED;
static trace_buffer_t log_buffer;

static void init_log_buffer(void)
{
	rb_init(&log_buffer.buf);
	atomic_set(&log_buffer.reader_cnt,0);
	init_MUTEX(&log_buffer.reader_mutex);
}

static DEFINE_PER_CPU(char[MSG_SIZE], fmt_buffer);

/*
 * sched_trace_log_message - Write to the trace buffer (log_buffer)
 *
 * This is the only function accessing the log_buffer from inside the
 * kernel for writing.
 * Concurrent access to sched_trace_log_message must be serialized using
 * log_buffer_lock
 * The maximum length of a formatted message is 255
 */
void sched_trace_log_message(const char* fmt, ...)
{
	unsigned long 	flags;
	va_list 	args;
	size_t		len;
	char*		buf;

	va_start(args, fmt);
	local_irq_save(flags);

	/* format message */
	buf = __get_cpu_var(fmt_buffer);
	len = vscnprintf(buf, MSG_SIZE, fmt, args);

	spin_lock(&log_buffer_lock);
	/* Don't copy the trailing null byte, we don't want null bytes
	 * in a text file.
	 */
	rb_put(&log_buffer.buf, buf, len);
	spin_unlock(&log_buffer_lock);

	local_irq_restore(flags);
	va_end(args);
}

/*
 * log_read - Read the trace buffer
 *
 * This function is called as a file operation from userspace.
 * Readers can sleep. Access is serialized through reader_mutex
 */
static ssize_t log_read(struct file *filp, char __user *to, size_t len,
		      loff_t *f_pos)
{
	/* we ignore f_pos, this is strictly sequential */

	ssize_t error = -EINVAL;
	char*   mem;
	trace_buffer_t *tbuf = filp->private_data;

	if (down_interruptible(&tbuf->reader_mutex)) {
		error = -ERESTARTSYS;
		goto out;
	}

	if (len > MAX_READ_LEN)
		len = MAX_READ_LEN;

	mem = kmalloc(len, GFP_KERNEL);
	if (!mem) {
		error = -ENOMEM;
		goto out_unlock;
	}

	error = rb_get(&tbuf->buf, mem, len);
	while (!error) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(110);
		if (signal_pending(current))
			error = -ERESTARTSYS;
		else
			error = rb_get(&tbuf->buf, mem, len);
	}

	if (error > 0 && copy_to_user(to, mem, error))
		error = -EFAULT;

	kfree(mem);
 out_unlock:
	up(&tbuf->reader_mutex);
 out:
	return error;
}

/*
 * Enable redirection of printk() messages to the trace buffer.
 * Defined in kernel/printk.c
 */
extern int trace_override;
extern int trace_recurse;

/*
 * log_open - open the global log message ring buffer.
 */
static int log_open(struct inode *in, struct file *filp)
{
	int error = -EINVAL;
	trace_buffer_t* tbuf;

	tbuf = &log_buffer;

	if (down_interruptible(&tbuf->reader_mutex)) {
		error = -ERESTARTSYS;
		goto out;
	}

	/* first open must allocate buffers */
	if (atomic_inc_return(&tbuf->reader_cnt) == 1) {
		if ((error = rb_alloc_buf(&tbuf->buf, LITMUS_TRACE_BUF_SIZE)))
		{
			atomic_dec(&tbuf->reader_cnt);
			goto out_unlock;
		}
	}

	error = 0;
	filp->private_data = tbuf;

	printk(KERN_DEBUG
	       "sched_trace kfifo at 0x%p with buffer starting at: 0x%p\n",
	       tbuf->buf.kfifo, &((tbuf->buf.kfifo)->buffer));

	/* override printk() */
	trace_override++;

 out_unlock:
	up(&tbuf->reader_mutex);
 out:
	return error;
}

static int log_release(struct inode *in, struct file *filp)
{
	int error = -EINVAL;
	trace_buffer_t* tbuf = filp->private_data;

	BUG_ON(!filp->private_data);

	if (down_interruptible(&tbuf->reader_mutex)) {
		error = -ERESTARTSYS;
		goto out;
	}

	/* last release must deallocate buffers */
	if (atomic_dec_return(&tbuf->reader_cnt) == 0) {
		error = rb_free_buf(&tbuf->buf);
	}

	/* release printk() overriding */
	trace_override--;

	printk(KERN_DEBUG "sched_trace kfifo released\n");

	up(&tbuf->reader_mutex);
 out:
	return error;
}

/*
 * log_fops  - The file operations for accessing the global LITMUS log message
 *             buffer.
 *
 * Except for opening the device file it uses the same operations as trace_fops.
 */
struct file_operations log_fops = {
	.owner   = THIS_MODULE,
	.open    = log_open,
	.release = log_release,
	.read    = log_read,
};

/*
 * Device registration
 */
static int __init register_buffer_dev(const char* name,
				      struct file_operations* fops,
				      int major, int count)
{
	dev_t trace_dev;
	struct cdev *cdev;
	int error = 0;

	if(major) {
		trace_dev = MKDEV(major, 0);
		error = register_chrdev_region(trace_dev, count, name);
	} else {
		/* dynamically allocate major number */
		error = alloc_chrdev_region(&trace_dev, 0, count, name);
		major = MAJOR(trace_dev);
	}
	if (error) {
		printk(KERN_WARNING "sched trace: "
		       "Could not register major/minor number %d\n", major);
		return error;
	}
	cdev = cdev_alloc();
	if (!cdev) {
		printk(KERN_WARNING "sched trace: "
			"Could not get a cdev for %s.\n", name);
		return -ENOMEM;
	}
	cdev->owner = THIS_MODULE;
	cdev->ops   = fops;
	error = cdev_add(cdev, trace_dev, count);
	if (error) {
		printk(KERN_WARNING "sched trace: "
			"add_cdev failed for %s.\n", name);
		return -ENOMEM;
	}
	return error;

}

#ifdef CONFIG_MAGIC_SYSRQ
void dump_trace_buffer(int max)
{
	char line[80];
	int len;
	int count = 0;

	/* potentially, but very unlikely race... */
	trace_recurse = 1;
	while ((max == 0 || count++ < max) &&
	       (len = rb_get(&log_buffer.buf, line, sizeof(line) - 1)) > 0) {
		line[len] = '\0';
		printk("%s", line);
	}
	trace_recurse = 0;
}

static void sysrq_dump_trace_buffer(int key, struct tty_struct *tty)
{
	dump_trace_buffer(100);
}

static struct sysrq_key_op sysrq_dump_trace_buffer_op = {
	.handler	= sysrq_dump_trace_buffer,
	.help_msg	= "dump-trace-buffer(Y)",
	.action_msg	= "writing content of TRACE() buffer",
};
#endif

static int __init init_sched_trace(void)
{
	printk("Initializing TRACE() device\n");
	init_log_buffer();

#ifdef CONFIG_MAGIC_SYSRQ
	/* offer some debugging help */
	if (!register_sysrq_key('y', &sysrq_dump_trace_buffer_op))
		printk("Registered dump-trace-buffer(Y) magic sysrq.\n");
	else
		printk("Could not register dump-trace-buffer(Y) magic sysrq.\n");
#endif


	return register_buffer_dev("litmus_log", &log_fops,
				   LOG_MAJOR, 1);
}

module_init(init_sched_trace);

