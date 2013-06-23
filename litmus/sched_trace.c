/*
 * sched_trace.c -- record scheduling events to a byte stream.
 */
#include <linux/spinlock.h>
#include <linux/mutex.h>

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/sysrq.h>
#include <linux/sched.h>
#include <linux/kfifo.h>

atomic_t __log_seq_no = ATOMIC_INIT(0);

#define SCHED_TRACE_NAME "litmus/log"

/* Compute size of TRACE() buffer */
#define LITMUS_TRACE_BUF_SIZE (1 << CONFIG_SCHED_DEBUG_TRACE_SHIFT)

/* Max length of one read from the buffer */
#define MAX_READ_LEN (64 * 1024)

/* Max length for one write --- by TRACE() --- to the buffer. This is used to
 * allocate a per-cpu buffer for printf() formatting. */
#define MSG_SIZE 255


static DEFINE_MUTEX(reader_mutex);
static atomic_t reader_cnt = ATOMIC_INIT(0);
static DEFINE_KFIFO(debug_buffer, char, LITMUS_TRACE_BUF_SIZE);


static DEFINE_RAW_SPINLOCK(log_buffer_lock);
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

	if (!atomic_read(&reader_cnt))
		/* early exit if nobody is listening */
		return;

	va_start(args, fmt);
	local_irq_save(flags);

	/* format message */
	buf = __get_cpu_var(fmt_buffer);
	len = vscnprintf(buf, MSG_SIZE, fmt, args);

	raw_spin_lock(&log_buffer_lock);
	/* Don't copy the trailing null byte, we don't want null bytes in a
	 * text file.
	 */
	kfifo_in(&debug_buffer, buf, len);
	raw_spin_unlock(&log_buffer_lock);

	local_irq_restore(flags);
	va_end(args);
}


/*
 * log_read - Read the trace buffer
 *
 * This function is called as a file operation from userspace.
 * Readers can sleep. Access is serialized through reader_mutex
 */
static ssize_t log_read(struct file *filp,
			char __user *to, size_t len,
			loff_t *f_pos)
{
	/* we ignore f_pos, this is strictly sequential */

	ssize_t error = -EINVAL;
	char* mem;

	if (mutex_lock_interruptible(&reader_mutex)) {
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

	error = kfifo_out(&debug_buffer, mem, len);
	while (!error) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(110);
		if (signal_pending(current))
			error = -ERESTARTSYS;
		else
			error = kfifo_out(&debug_buffer, mem, len);
	}

	if (error > 0 && copy_to_user(to, mem, error))
		error = -EFAULT;

	kfree(mem);
 out_unlock:
	mutex_unlock(&reader_mutex);
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

	if (mutex_lock_interruptible(&reader_mutex)) {
		error = -ERESTARTSYS;
		goto out;
	}

	atomic_inc(&reader_cnt);
	error = 0;

	printk(KERN_DEBUG
	       "sched_trace kfifo with buffer starting at: 0x%p\n",
	       debug_buffer.buf);

	/* override printk() */
	trace_override++;

	mutex_unlock(&reader_mutex);
 out:
	return error;
}

static int log_release(struct inode *in, struct file *filp)
{
	int error = -EINVAL;

	if (mutex_lock_interruptible(&reader_mutex)) {
		error = -ERESTARTSYS;
		goto out;
	}

	atomic_dec(&reader_cnt);

	/* release printk() overriding */
	trace_override--;

	printk(KERN_DEBUG "sched_trace kfifo released\n");

	mutex_unlock(&reader_mutex);
 out:
	return error;
}

/*
 * log_fops  - The file operations for accessing the global LITMUS log message
 *             buffer.
 *
 * Except for opening the device file it uses the same operations as trace_fops.
 */
static struct file_operations log_fops = {
	.owner   = THIS_MODULE,
	.open    = log_open,
	.release = log_release,
	.read    = log_read,
};

static struct miscdevice litmus_log_dev = {
	.name    = SCHED_TRACE_NAME,
	.minor   = MISC_DYNAMIC_MINOR,
	.fops    = &log_fops,
};

#ifdef CONFIG_MAGIC_SYSRQ
void dump_trace_buffer(int max)
{
	char line[80];
	int len;
	int count = 0;

	/* potential, but very unlikely, race... */
	trace_recurse = 1;
	while ((max == 0 || count++ < max) &&
	       (len = kfifo_out(&debug_buffer, line, sizeof(line - 1))) > 0) {
		line[len] = '\0';
		printk("%s", line);
	}
	trace_recurse = 0;
}

static void sysrq_dump_trace_buffer(int key)
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

#ifdef CONFIG_MAGIC_SYSRQ
	/* offer some debugging help */
	if (!register_sysrq_key('y', &sysrq_dump_trace_buffer_op))
		printk("Registered dump-trace-buffer(Y) magic sysrq.\n");
	else
		printk("Could not register dump-trace-buffer(Y) magic sysrq.\n");
#endif

	return misc_register(&litmus_log_dev);
}

static void __exit exit_sched_trace(void)
{
	misc_deregister(&litmus_log_dev);
}

module_init(init_sched_trace);
module_exit(exit_sched_trace);
