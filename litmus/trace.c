#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>
#include <linux/module.h>

#include <litmus/ftdev.h>

#include <litmus/litmus.h>
#include <litmus/trace.h>

/******************************************************************************/
/*                          Allocation                                        */
/******************************************************************************/

static struct ftdev overhead_dev;

#define trace_ts_buf overhead_dev.minor[0].buf

static unsigned int ts_seq_no = 0;

static inline void __save_timestamp(unsigned long event, uint8_t type)
{
	unsigned int seq_no;
	struct timestamp *ts;
	seq_no = fetch_and_inc((int *) &ts_seq_no);
	if (ft_buffer_start_write(trace_ts_buf, (void**)  &ts)) {
		ts->event     = event;
		ts->timestamp = ft_timestamp();
		ts->seq_no    = seq_no;
		ts->cpu       = raw_smp_processor_id();
		ts->task_type = type;
		ft_buffer_finish_write(trace_ts_buf, ts);
	}
}

feather_callback void save_timestamp(unsigned long event)
{
	__save_timestamp(event, TSK_UNKNOWN);
}

feather_callback void save_timestamp_def(unsigned long event, unsigned long type)
{
	__save_timestamp(event, (uint8_t) type);
}

feather_callback void save_timestamp_task(unsigned long event, unsigned long t_ptr)
{
	int rt = is_realtime((struct task_struct *) t_ptr);
	__save_timestamp(event, rt ? TSK_RT : TSK_BE);
}

/******************************************************************************/
/*                        DEVICE FILE DRIVER                                  */
/******************************************************************************/

#define NO_TIMESTAMPS (2 << 19) /* that should be 8 megs of ram, we may not get
				 * as much */
#define FT_TRACE_MAJOR	252

static int alloc_timestamp_buffer(struct ftdev* ftdev, unsigned int idx)
{
	unsigned int count = NO_TIMESTAMPS;
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

static int __init init_ft_overhead_trace(void)
{
	printk("Initializing Feather-Trace overhead tracing device.\n");
	ftdev_init(&overhead_dev);
	overhead_dev.minor_cnt = 1; /* only one buffer */
	overhead_dev.alloc = alloc_timestamp_buffer;
	overhead_dev.free  = free_timestamp_buffer;
	return register_ftdev(&overhead_dev, "ft_trace", FT_TRACE_MAJOR);
}

module_init(init_ft_overhead_trace);
