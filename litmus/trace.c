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
static struct ftdev cpu_overhead_dev;

#define trace_ts_buf overhead_dev.minor[0].buf

#define cpu_trace_ts_buf(cpu) cpu_overhead_dev.minor[(cpu)].buf

static unsigned int ts_seq_no = 0;

DEFINE_PER_CPU(atomic_t, irq_fired_count;)
DEFINE_PER_CPU(atomic_t, cpu_irq_fired_count);

static DEFINE_PER_CPU(unsigned int, cpu_ts_seq_no);

void ft_irq_fired(void)
{
	/* Only called with preemptions disabled.  */
	atomic_inc(&__get_cpu_var(irq_fired_count));
	atomic_inc(&__get_cpu_var(cpu_irq_fired_count));

	if (has_control_page(current))
		get_control_page(current)->irq_count++;
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

static inline unsigned int get_and_clear_irq_fired_for_cpu(int cpu)
{
	return atomic_xchg(&per_cpu(irq_fired_count, cpu), 0);
}

static inline void cpu_clear_irq_fired(void)
{
	atomic_set(&__raw_get_cpu_var(cpu_irq_fired_count), 0);
}

static inline unsigned int cpu_get_and_clear_irq_fired(void)
{
	return atomic_xchg(&__raw_get_cpu_var(cpu_irq_fired_count), 0);
}

static inline void save_irq_flags(struct timestamp *ts, unsigned int irq_count)
{
	/* Store how many interrupts occurred. */
	ts->irq_count = irq_count;
	/* Extra flag because ts->irq_count overflows quickly. */
	ts->irq_flag  = irq_count > 0;
}

#define NO_IRQ_COUNT 0
#define LOCAL_IRQ_COUNT 1
#define REMOTE_IRQ_COUNT 2

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

		if (likely(record_irq == LOCAL_IRQ_COUNT))
			irq_count = get_and_clear_irq_fired();
		else if (record_irq == REMOTE_IRQ_COUNT)
			irq_count = get_and_clear_irq_fired_for_cpu(cpu);

		save_irq_flags(ts, irq_count - hide_irq);

		if (record_timestamp)
			timestamp = ft_timestamp();

		ts->timestamp = timestamp;
		ft_buffer_finish_write(trace_ts_buf, ts);
	}

	local_irq_restore(flags);
}

static inline void write_cpu_timestamp(
	uint8_t event,
	uint8_t type,
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
	int cpu;
	struct ft_buffer* buf;

	local_irq_save(flags);
	cpu = smp_processor_id();

	seq_no = __get_cpu_var(cpu_ts_seq_no)++;

	buf = cpu_trace_ts_buf(cpu);
	/* If buf is non-NULL here, then the buffer cannot be deallocated until
	 * we turn interrupts on again. This is because free_timestamp_buffer()
	 * indirectly causes TLB invalidations due to modifications of the
	 * kernel address space, namely via vfree() in free_ft_buffer(), which
	 * cannot be processed until we turn on interrupts again.
	 */

	if (buf && ft_buffer_start_write(buf, (void**)  &ts)) {
		ts->event     = event;
		ts->seq_no    = seq_no;

		ts->task_type = type;
		ts->pid	      = pid_fragment;

		ts->cpu       = cpu;

		if (record_irq)
			irq_count = cpu_get_and_clear_irq_fired();

		save_irq_flags(ts, irq_count - hide_irq);

		if (record_timestamp)
			timestamp = ft_timestamp();

		ts->timestamp = timestamp;
		ft_buffer_finish_write(buf, ts);
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

feather_callback void save_cpu_timestamp_def(unsigned long event,
					     unsigned long type)
{
	write_cpu_timestamp(event, type,
			    current->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    0, 1);
}

feather_callback void save_cpu_timestamp_task(unsigned long event,
					      unsigned long t_ptr)
{
	struct task_struct *t = (struct task_struct *) t_ptr;
	int rt = is_realtime(t);

	write_cpu_timestamp(event, rt ? TSK_RT : TSK_BE,
			    t->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    0, 1);
}

feather_callback void save_cpu_task_latency(unsigned long event,
					    unsigned long when_ptr)
{
	lt_t now = litmus_clock();
	lt_t *when = (lt_t*) when_ptr;

	write_cpu_timestamp(event, TSK_RT,
			    0,
			    0, LOCAL_IRQ_COUNT, 0,
			    now - *when, 0);
}

/* fake timestamp to user-reported time */
feather_callback void save_cpu_timestamp_time(unsigned long event,
			 unsigned long ptr)
{
	uint64_t* time = (uint64_t*) ptr;

	write_cpu_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			    current->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    *time, 0);
}

/* Record user-reported IRQ count */
feather_callback void save_cpu_timestamp_irq(unsigned long event,
			unsigned long irq_counter_ptr)
{
	uint64_t* irqs = (uint64_t*) irq_counter_ptr;

	write_cpu_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			    current->pid,
			    *irqs, NO_IRQ_COUNT, 0,
			    0, 1);
}

feather_callback void save_timestamp_cpu(unsigned long event,
					 unsigned long cpu)
{
	write_timestamp(event, TSK_UNKNOWN, cpu, current->pid,
			0, REMOTE_IRQ_COUNT, 0,
			0, 1);
}

/* Suppress one IRQ from the irq count. Used by TS_SEND_RESCHED_END, which is
 * called from within an interrupt that is expected. */
feather_callback void save_timestamp_hide_irq(unsigned long event)
{
	write_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			raw_smp_processor_id(), current->pid,
			0, LOCAL_IRQ_COUNT, 1,
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

	while (count && !ftdev->minor[idx].buf) {
		printk("time stamp buffer: trying to allocate %u time stamps for minor=%u.\n", count, idx);
		ftdev->minor[idx].buf = alloc_ft_buffer(count, sizeof(struct timestamp));
		count /= 2;
	}
	return ftdev->minor[idx].buf ? 0 : -ENOMEM;
}

static void free_timestamp_buffer(struct ftdev* ftdev, unsigned int idx)
{
	ftdev->minor[idx].buf = NULL;
	/* Make sure all cores have actually seen buf == NULL before
	 * yanking out the mappings from underneath them. */
	smp_wmb();
	free_ft_buffer(ftdev->minor[idx].buf);
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


static int __init init_global_ft_overhead_trace(void)
{
	int err;

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

	return 0;

err_dealloc:
	ftdev_exit(&overhead_dev);
err_out:
	printk(KERN_WARNING "Could not register ft_trace module.\n");
	return err;
}

static int __init init_cpu_ft_overhead_trace(void)
{
	int err, cpu;

	printk("Initializing Feather-Trace per-cpu overhead tracing device.\n");
	err = ftdev_init(&cpu_overhead_dev, THIS_MODULE,
			 num_online_cpus(), "ft_cpu_trace");
	if (err)
		goto err_out;

	cpu_overhead_dev.alloc = alloc_timestamp_buffer;
	cpu_overhead_dev.free  = free_timestamp_buffer;

	err = register_ftdev(&cpu_overhead_dev);
	if (err)
		goto err_dealloc;

	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		per_cpu(cpu_ts_seq_no, cpu) = 0;
	}

	return 0;

err_dealloc:
	ftdev_exit(&cpu_overhead_dev);
err_out:
	printk(KERN_WARNING "Could not register per-cpu ft_trace module.\n");
	return err;
}


static int __init init_ft_overhead_trace(void)
{
	int err;

	err = init_global_ft_overhead_trace();
	if (err)
		return err;

	err = init_cpu_ft_overhead_trace();
	if (err) {
		ftdev_exit(&overhead_dev);
		return err;
	}

	return 0;
}

static void __exit exit_ft_overhead_trace(void)
{
	ftdev_exit(&overhead_dev);
	ftdev_exit(&cpu_overhead_dev);
}

module_init(init_ft_overhead_trace);
module_exit(exit_ft_overhead_trace);
