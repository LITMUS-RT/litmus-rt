#include <linux/sched.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include <litmus/ftdev.h>
#include <litmus/litmus.h>
#include <litmus/trace.h>

/******************************************************************************/
/*                          Allocation                                        */
/******************************************************************************/

static struct ftdev overhead_dev;

#define trace_ts_buf overhead_dev.minor[0].buf

static unsigned int ts_seq_no = 0;

DEFINE_PER_CPU(atomic_t, irq_fired_count);

void ft_irq_fired(void)
{
	/* Only called with preemptions disabled.  */
	atomic_inc(&__get_cpu_var(irq_fired_count));
}

static inline void clear_irq_fired(void)
{
	atomic_set(&__raw_get_cpu_var(irq_fired_count), 0);
}

static inline unsigned int get_and_clear_irq_fired(void)
{
	/* This is potentially not atomic  since we might migrate if
	 * preemptions are not disabled. As a tradeoff between
	 * accuracy and tracing overheads, this seems acceptable.
	 * If it proves to be a problem, then one could add a callback
	 * from the migration code to invalidate irq_fired_count.
	 */
	return atomic_xchg(&__raw_get_cpu_var(irq_fired_count), 0);
}

static inline void save_irq_flags(struct timestamp *ts, unsigned int irq_count)
{
	/* Store how many interrupts occurred. */
	ts->irq_count = irq_count;
	/* Extra flag because ts->irq_count overflows quickly. */
	ts->irq_flag  = irq_count > 0;

}

static inline void write_timestamp(uint8_t event,
				   uint8_t type,
				   uint8_t cpu,
				   uint16_t pid_fragment,
				   unsigned int irq_count,
				   int record_irq,
				   int hide_irq,
				   uint64_t timestamp,
				   int record_timestamp)
{
	unsigned long flags;
	unsigned int seq_no;
	struct timestamp *ts;

	/* Avoid preemptions while recording the timestamp. This reduces the
	 * number of "out of order" timestamps in the stream and makes
	 * post-processing easier. */

	local_irq_save(flags);

	seq_no = fetch_and_inc((int *) &ts_seq_no);
	if (ft_buffer_start_write(trace_ts_buf, (void**)  &ts)) {
		ts->event     = event;
		ts->seq_no    = seq_no;

		ts->task_type = type;
		ts->pid	      = pid_fragment;

		ts->cpu       = cpu;

		if (record_irq)
			irq_count = get_and_clear_irq_fired();

		save_irq_flags(ts, irq_count - hide_irq);

		if (record_timestamp)
			timestamp = ft_timestamp();

		ts->timestamp = timestamp;
		ft_buffer_finish_write(trace_ts_buf, ts);
	}

	local_irq_restore(flags);
}

static void __add_timestamp_user(struct timestamp *pre_recorded)
{
	unsigned long flags;
	unsigned int seq_no;
	struct timestamp *ts;


	local_irq_save(flags);

	seq_no = fetch_and_inc((int *) &ts_seq_no);
	if (ft_buffer_start_write(trace_ts_buf, (void**)  &ts)) {
		*ts = *pre_recorded;
		ts->seq_no = seq_no;
		ts->cpu	   = raw_smp_processor_id();
	        save_irq_flags(ts, get_and_clear_irq_fired());
		ft_buffer_finish_write(trace_ts_buf, ts);
	}

	local_irq_restore(flags);
}

feather_callback void save_timestamp(unsigned long event)
{
	write_timestamp(event, TSK_UNKNOWN,
			raw_smp_processor_id(),
			current->pid,
			0, 1, 0,
			0, 1);
}

feather_callback void save_timestamp_def(unsigned long event,
					 unsigned long type)
{
	write_timestamp(event, type,
			raw_smp_processor_id(),
			current->pid,
			0, 1, 0,
			0, 1);
}

feather_callback void save_timestamp_task(unsigned long event,
					  unsigned long t_ptr)
{
	struct task_struct *t = (struct task_struct *) t_ptr;
	int rt = is_realtime(t);

	write_timestamp(event, rt ? TSK_RT : TSK_BE,
			raw_smp_processor_id(),
			t->pid,
			0, 1, 0,
			0, 1);
}

feather_callback void save_timestamp_cpu(unsigned long event,
					 unsigned long cpu)
{
	write_timestamp(event, TSK_UNKNOWN, cpu, current->pid,
			0, 1, 0,
			0, 1);
}

/* fake timestamp to user-reported time */
feather_callback void save_timestamp_time(unsigned long event,
			 unsigned long ptr)
{
	uint64_t* time = (uint64_t*) ptr;

	write_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			raw_smp_processor_id(), current->pid,
			0, 1, 0,
			*time, 0);
}

/* Record user-reported IRQ count */
feather_callback void save_timestamp_irq(unsigned long event,
			unsigned long irq_counter_ptr)
{
	uint64_t* irqs = (uint64_t*) irq_counter_ptr;

	write_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			raw_smp_processor_id(), current->pid,
			*irqs, 0, 0,
			0, 1);
}

/* Suppress one IRQ from the irq count. Used by TS_SEND_RESCHED_END, which is
 * called from within an interrupt that is expected. */
feather_callback void save_timestamp_hide_irq(unsigned long event)
{
	write_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			raw_smp_processor_id(), current->pid,
			0, 1, 1,
			0, 1);
}

/******************************************************************************/
/*                        DEVICE FILE DRIVER                                  */
/******************************************************************************/

#define NO_TIMESTAMPS (2 << CONFIG_SCHED_OVERHEAD_TRACE_SHIFT)

static int alloc_timestamp_buffer(struct ftdev* ftdev, unsigned int idx)
{
	unsigned int count = NO_TIMESTAMPS;

	/* An overhead-tracing timestamp should be exactly 16 bytes long. */
	BUILD_BUG_ON(sizeof(struct timestamp) != 16);

	while (count && !trace_ts_buf) {
		printk("time stamp buffer: trying to allocate %u time stamps.\n", count);
		ftdev->minor[idx].buf = alloc_ft_buffer(count, sizeof(struct timestamp));
		count /= 2;
	}
	return ftdev->minor[idx].buf ? 0 : -ENOMEM;
}

static void free_timestamp_buffer(struct ftdev* ftdev, unsigned int idx)
{
	free_ft_buffer(ftdev->minor[idx].buf);
	ftdev->minor[idx].buf = NULL;
}

static ssize_t write_timestamp_from_user(struct ft_buffer* buf, size_t len,
					 const char __user *from)
{
	ssize_t consumed = 0;
	struct timestamp ts;

	/* don't give us partial timestamps */
	if (len % sizeof(ts))
		return -EINVAL;

	while (len >= sizeof(ts)) {
		if (copy_from_user(&ts, from, sizeof(ts))) {
			consumed = -EFAULT;
			goto out;
		}
		len  -= sizeof(ts);
		from += sizeof(ts);
		consumed += sizeof(ts);

		__add_timestamp_user(&ts);
	}

out:
	return consumed;
}

static int __init init_ft_overhead_trace(void)
{
	int err, cpu;

	printk("Initializing Feather-Trace overhead tracing device.\n");
	err = ftdev_init(&overhead_dev, THIS_MODULE, 1, "ft_trace");
	if (err)
		goto err_out;

	overhead_dev.alloc = alloc_timestamp_buffer;
	overhead_dev.free  = free_timestamp_buffer;
	overhead_dev.write = write_timestamp_from_user;

	err = register_ftdev(&overhead_dev);
	if (err)
		goto err_dealloc;

	/* initialize IRQ flags */
	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		clear_irq_fired();
	}

	return 0;

err_dealloc:
	ftdev_exit(&overhead_dev);
err_out:
	printk(KERN_WARNING "Could not register ft_trace module.\n");
	return err;
}

static void __exit exit_ft_overhead_trace(void)
{
	ftdev_exit(&overhead_dev);
}

module_init(init_ft_overhead_trace);
module_exit(exit_ft_overhead_trace);
