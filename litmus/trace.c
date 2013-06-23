#include <linux/sched.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include <litmus/ftdev.h>
#include <litmus/trace.h>

/* dummy definition of is_realtime() */
#define is_realtime(t) (0)

/******************************************************************************/
/*                          Allocation                                        */
/******************************************************************************/

static struct ftdev cpu_overhead_dev;
static struct ftdev msg_overhead_dev;

#define cpu_trace_ts_buf(cpu) cpu_overhead_dev.minor[(cpu)].buf
#define msg_trace_ts_buf(cpu) msg_overhead_dev.minor[(cpu)].buf

DEFINE_PER_CPU(atomic_t, irq_fired_count;)
DEFINE_PER_CPU_SHARED_ALIGNED(atomic_t, cpu_irq_fired_count);

static DEFINE_PER_CPU(unsigned int, cpu_ts_seq_no);
static DEFINE_PER_CPU(unsigned int, msg_ts_seq_no);

static int64_t cycle_offset[NR_CPUS][NR_CPUS];

void ft_irq_fired(void)
{
	/* Only called with preemptions disabled.  */
	atomic_inc(&__get_cpu_var(irq_fired_count));
	atomic_inc(&__get_cpu_var(cpu_irq_fired_count));
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

#define DO_NOT_RECORD_TIMESTAMP 0
#define RECORD_LOCAL_TIMESTAMP 1
#define RECORD_OFFSET_TIMESTAMP 2

static inline void __write_record(
	uint8_t event,
	uint8_t type,
	uint16_t pid_fragment,
	unsigned int irq_count,
	int record_irq,
	int hide_irq,
	uint64_t timestamp,
	int record_timestamp,

	int only_single_writer,
	int is_cpu_timestamp,
	int local_cpu,
	uint8_t other_cpu)
{
	unsigned long flags;
	unsigned int seq_no;
	struct timestamp *ts;
	int cpu;
	struct ft_buffer* buf;

	/* Avoid preemptions while recording the timestamp. This reduces the
	 * number of "out of order" timestamps in the stream and makes
	 * post-processing easier. */

	local_irq_save(flags);

	if (local_cpu)
		cpu = smp_processor_id();
	else
		cpu = other_cpu;

	/* resolved during function inlining */
	if (is_cpu_timestamp) {
		seq_no = __get_cpu_var(cpu_ts_seq_no)++;
		buf = cpu_trace_ts_buf(cpu);
	} else {
		seq_no = fetch_and_inc((int *) &per_cpu(msg_ts_seq_no, cpu));
		buf = msg_trace_ts_buf(cpu);
	}

	/* If buf is non-NULL here, then the buffer cannot be deallocated until
	 * we turn interrupts on again. This is because free_timestamp_buffer()
	 * indirectly causes TLB invalidations due to modifications of the
	 * kernel address space, namely via vfree() in free_ft_buffer(), which
	 * cannot be processed until we turn on interrupts again.
	 */

	if (buf &&
	    (only_single_writer /* resolved during function inlining */
	     ? ft_buffer_start_single_write(buf, (void**)  &ts)
	     : ft_buffer_start_write(buf, (void**) &ts))) {
		ts->event     = event;
		ts->seq_no    = seq_no;

		ts->task_type = type;
		ts->pid	      = pid_fragment;

		ts->cpu       = cpu;

		if (record_irq) {
			if (local_cpu)
				irq_count = cpu_get_and_clear_irq_fired();
			else
				irq_count = get_and_clear_irq_fired_for_cpu(cpu);
		}

		save_irq_flags(ts, irq_count - hide_irq);

		if (record_timestamp)
			timestamp = ft_timestamp();
		if (record_timestamp == RECORD_OFFSET_TIMESTAMP)
			timestamp += cycle_offset[smp_processor_id()][cpu];

		ts->timestamp = timestamp;
		ft_buffer_finish_write(buf, ts);
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
	__write_record(event, type,
		       pid_fragment,
		       irq_count, record_irq, hide_irq,
		       timestamp, record_timestamp,
		       1 /* only_single_writer */,
		       1 /* is_cpu_timestamp */,
		       1 /* local_cpu */,
		       0xff /* other_cpu */);
}

static inline void save_msg_timestamp(
	uint8_t event,
	int hide_irq)
{
	struct task_struct *t  = current;
	__write_record(event, is_realtime(t) ? TSK_RT : TSK_BE,
		       t->pid,
		       0, LOCAL_IRQ_COUNT, hide_irq,
		       0, RECORD_LOCAL_TIMESTAMP,
		       0 /* only_single_writer */,
		       0 /* is_cpu_timestamp */,
		       1 /* local_cpu */,
		       0xff /* other_cpu */);
}

static inline void save_remote_msg_timestamp(
	uint8_t event,
	uint8_t remote_cpu)
{
	struct task_struct *t  = current;
	__write_record(event, is_realtime(t) ? TSK_RT : TSK_BE,
		       t->pid,
		       0, REMOTE_IRQ_COUNT, 0,
		       0, RECORD_OFFSET_TIMESTAMP,
		       0 /* only_single_writer */,
		       0 /* is_cpu_timestamp */,
		       0 /* local_cpu */,
		       remote_cpu);
}

feather_callback void save_cpu_timestamp_def(unsigned long event,
					     unsigned long type)
{
	write_cpu_timestamp(event, type,
			    current->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    0, RECORD_LOCAL_TIMESTAMP);
}

feather_callback void save_cpu_timestamp_task(unsigned long event,
					      unsigned long t_ptr)
{
	struct task_struct *t = (struct task_struct *) t_ptr;
	int rt = is_realtime(t);

	write_cpu_timestamp(event, rt ? TSK_RT : TSK_BE,
			    t->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    0, RECORD_LOCAL_TIMESTAMP);
}

feather_callback void save_cpu_task_latency(unsigned long event,
					    unsigned long when_ptr)
{
	lt_t now = litmus_clock();
	lt_t *when = (lt_t*) when_ptr;

	write_cpu_timestamp(event, TSK_RT,
			    0,
			    0, LOCAL_IRQ_COUNT, 0,
			    now - *when, DO_NOT_RECORD_TIMESTAMP);
}

/* fake timestamp to user-reported time */
feather_callback void save_cpu_timestamp_time(unsigned long event,
			 unsigned long ptr)
{
	uint64_t* time = (uint64_t*) ptr;

	write_cpu_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			    current->pid,
			    0, LOCAL_IRQ_COUNT, 0,
			    *time, DO_NOT_RECORD_TIMESTAMP);
}

/* Record user-reported IRQ count */
feather_callback void save_cpu_timestamp_irq(unsigned long event,
			unsigned long irq_counter_ptr)
{
	uint64_t* irqs = (uint64_t*) irq_counter_ptr;

	write_cpu_timestamp(event, is_realtime(current) ? TSK_RT : TSK_BE,
			    current->pid,
			    *irqs, NO_IRQ_COUNT, 0,
			    0, RECORD_LOCAL_TIMESTAMP);
}


feather_callback void msg_sent(unsigned long event, unsigned long to)
{
	save_remote_msg_timestamp(event, to);
}

/* Suppresses one IRQ from the irq count. Used by TS_SEND_RESCHED_END, which is
 * called from within an interrupt that is expected. */
feather_callback void msg_received(unsigned long event)
{
	save_msg_timestamp(event, 1);
}

static void __add_timestamp_user(struct timestamp *pre_recorded)
{
	unsigned long flags;
	unsigned int seq_no;
	struct timestamp *ts;
	struct ft_buffer* buf;
	int cpu;

	local_irq_save(flags);

	cpu = smp_processor_id();
	buf = cpu_trace_ts_buf(cpu);

	seq_no = __get_cpu_var(cpu_ts_seq_no)++;
	if (buf && ft_buffer_start_single_write(buf, (void**)  &ts)) {
		*ts = *pre_recorded;
		ts->seq_no = seq_no;
		ts->cpu	   = raw_smp_processor_id();
	        save_irq_flags(ts, get_and_clear_irq_fired());
		ft_buffer_finish_write(buf, ts);
	}

	local_irq_restore(flags);
}

/******************************************************************************/
/*                        DEVICE FILE DRIVER                                  */
/******************************************************************************/

struct calibrate_info {
	atomic_t ready;

	uint64_t cycle_count;
};

static void calibrate_helper(void *_info)
{
	struct calibrate_info *info = _info;
	/* check in with master */
	atomic_inc(&info->ready);

	/* wait for master to signal start */
	while (atomic_read(&info->ready))
		cpu_relax();

	/* report time stamp */
	info->cycle_count = ft_timestamp();

	/* tell master that we are done */
	atomic_inc(&info->ready);
}


static int64_t calibrate_cpu(int cpu)
{
	uint64_t cycles;
	struct calibrate_info info;
	unsigned long flags;
	int64_t  delta;

	atomic_set(&info.ready, 0);
	info.cycle_count = 0;
	smp_wmb();

	smp_call_function_single(cpu, calibrate_helper, &info, 0);

	/* wait for helper to become active */
	while (!atomic_read(&info.ready))
		cpu_relax();

	/* avoid interrupt interference */
	local_irq_save(flags);

	/* take measurement */
	atomic_set(&info.ready, 0);
	smp_wmb();
	cycles = ft_timestamp();

	/* wait for helper reading */
	while (!atomic_read(&info.ready))
		cpu_relax();

	/* positive offset: the other guy is ahead of us */
	delta  = (int64_t) info.cycle_count;
	delta -= (int64_t) cycles;

	local_irq_restore(flags);

	return delta;
}

#define NUM_SAMPLES 10

static long calibrate_tsc_offsets(struct ftdev* ftdev, unsigned int idx,
				  unsigned long uarg)
{
	int cpu, self, i;
	int64_t delta, sample;

	preempt_disable();
	self = smp_processor_id();

	if (uarg)
		printk(KERN_INFO "Feather-Trace: determining TSC offsets for P%d\n", self);

	for_each_online_cpu(cpu)
		if (cpu != self) {
			delta = calibrate_cpu(cpu);
			for (i = 1; i < NUM_SAMPLES; i++) {
			        sample = calibrate_cpu(cpu);
				delta = sample < delta ? sample : delta;
			}

			cycle_offset[self][cpu] = delta;

			if (uarg)
				printk(KERN_INFO "Feather-Trace: TSC offset for P%d->P%d is %lld cycles.\n",
				       self, cpu, cycle_offset[self][cpu]);
		}

	preempt_enable();
	return 0;
}

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

		/* Note: this always adds to the buffer of the CPU-local
		 * device, not necessarily to the device that the system call
		 * was invoked on. This is admittedly a bit ugly, but requiring
		 * tasks to only write to the appropriate device would make
		 * tracing from userspace under global and clustered scheduling
		 * exceedingly difficult. Writing to remote buffers would
		 * require to not use ft_buffer_start_single_write(), which we
		 * want to do to reduce the number of atomic ops in the common
		 * case (which is the recording of CPU-local scheduling
		 * overheads).
		 */
		__add_timestamp_user(&ts);
	}

out:
	return consumed;
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
	cpu_overhead_dev.write = write_timestamp_from_user;

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
	printk(KERN_WARNING "Could not register per-cpu ft_trace device.\n");
	return err;
}

static int __init init_msg_ft_overhead_trace(void)
{
	int err, cpu;

	printk("Initializing Feather-Trace per-cpu message overhead tracing device.\n");
	err = ftdev_init(&msg_overhead_dev, THIS_MODULE,
			 num_online_cpus(), "ft_msg_trace");
	if (err)
		goto err_out;

	msg_overhead_dev.alloc = alloc_timestamp_buffer;
	msg_overhead_dev.free  = free_timestamp_buffer;
	msg_overhead_dev.calibrate = calibrate_tsc_offsets;

	err = register_ftdev(&msg_overhead_dev);
	if (err)
		goto err_dealloc;

	for (cpu = 0; cpu < NR_CPUS; cpu++)  {
		per_cpu(msg_ts_seq_no, cpu) = 0;
	}

	return 0;

err_dealloc:
	ftdev_exit(&msg_overhead_dev);
err_out:
	printk(KERN_WARNING "Could not register message ft_trace device.\n");
	return err;
}


static int __init init_ft_overhead_trace(void)
{
	int err, i, j;

	for (i = 0; i < NR_CPUS; i++)
		for (j = 0; j < NR_CPUS; j++)
			cycle_offset[i][j] = 0;

	err = init_cpu_ft_overhead_trace();
	if (err)
		return err;

	err = init_msg_ft_overhead_trace();
	if (err)
		ftdev_exit(&cpu_overhead_dev);
		return err;

	return 0;
}

static void __exit exit_ft_overhead_trace(void)
{
	ftdev_exit(&cpu_overhead_dev);
	ftdev_exit(&msg_overhead_dev);
}

module_init(init_ft_overhead_trace);
module_exit(exit_ft_overhead_trace);
