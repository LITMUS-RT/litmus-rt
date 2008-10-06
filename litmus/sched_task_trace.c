/* sched_task_trace.c -- record scheduling events to a byte stream
 *
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/percpu.h>

#include <litmus/ftdev.h>
#include <litmus/litmus.h>

#include <litmus/sched_trace.h>
#include <litmus/feather_trace.h>
#include <litmus/ftdev.h>

#define FT_TASK_TRACE_MAJOR	253
#define NO_EVENTS		4096 /* this is a buffer of 12 4k pages per CPU */

struct local_buffer {
	struct st_event_record record[NO_EVENTS];
	char   flag[NO_EVENTS];
	struct ft_buffer ftbuf;
};

DEFINE_PER_CPU(struct local_buffer, st_event_buffer);

static struct ftdev st_dev;

static int st_dev_can_open(struct ftdev *dev, unsigned int cpu)
{
	return cpu_online(cpu) ? 0 : -ENODEV;
}

static int __init init_sched_task_trace(void)
{
	struct local_buffer* buf;
	int i, ok = 0;
	ftdev_init(&st_dev, THIS_MODULE);
	for (i = 0; i < NR_CPUS; i++) {
		buf = &per_cpu(st_event_buffer, i);
		ok += init_ft_buffer(&buf->ftbuf, NO_EVENTS,
				     sizeof(struct st_event_record),
				     buf->flag,
				     buf->record);
		st_dev.minor[i].buf = &buf->ftbuf;
	}
	if (ok == NR_CPUS) {
		st_dev.minor_cnt = NR_CPUS;
		st_dev.can_open = st_dev_can_open;
		return register_ftdev(&st_dev, "sched_trace", FT_TASK_TRACE_MAJOR);
	} else
		return -EINVAL;
}

module_init(init_sched_task_trace);


static inline struct st_event_record* get_record(u8 type, struct task_struct* t)
{
	struct st_event_record* rec;
	struct local_buffer* buf;

	buf = &get_cpu_var(st_event_buffer);
	if (ft_buffer_start_write(&buf->ftbuf, (void**) &rec)) {
		rec->hdr.type = type;
		rec->hdr.cpu  = smp_processor_id();
		rec->hdr.pid  = t ? t->pid : 0;
		rec->hdr.job  = t ? t->rt_param.job_params.job_no : -1;
	} else
		put_cpu_var(st_event_buffer);
	/* rec will be NULL if it failed */
	return rec;
}

static inline void put_record(struct st_event_record* rec)
{
	struct local_buffer* buf;
	buf = &__get_cpu_var(st_event_buffer);
	ft_buffer_finish_write(&buf->ftbuf, rec);
	put_cpu_var(st_event_buffer);
}

feather_callback void do_sched_trace_task_name(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_NAME, t);
	int i;
	if (rec) {
		for (i = 0; i < min(TASK_COMM_LEN, ST_NAME_LEN); i++)
			rec->data.name.cmd[i] = t->comm[i];
		put_record(rec);
	}
}

feather_callback void do_sched_trace_task_param(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_PARAM, t);
	if (rec) {
		rec->data.param.wcet      = get_exec_cost(t);
		rec->data.param.period    = get_rt_period(t);
		rec->data.param.phase     = get_rt_phase(t);
		rec->data.param.partition = get_partition(t);
		put_record(rec);
	}
}

feather_callback void do_sched_trace_task_release(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_RELEASE, t);
	if (rec) {
		rec->data.release.release  = get_release(t);
		rec->data.release.deadline = get_deadline(t);
		put_record(rec);
	}
}

/* skipped: st_assigned_data, we don't use it atm */

feather_callback void do_sched_trace_task_switch_to(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec;
	if (is_realtime(t)) {
		rec = get_record(ST_SWITCH_TO, t);
		if (rec) {
			rec->data.switch_to.when      = sched_clock();
			rec->data.switch_to.exec_time = get_exec_time(t);
			put_record(rec);
		}
	}
}

feather_callback void do_sched_trace_task_switch_away(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec;
	if (is_realtime(t)) {
		rec = get_record(ST_SWITCH_AWAY, t);
		if (rec) {
			rec->data.switch_away.when      = sched_clock();
			rec->data.switch_away.exec_time = get_exec_time(t);
			put_record(rec);
		}
	}
}

feather_callback void do_sched_trace_task_completion(unsigned long id, unsigned long _task, 
						     unsigned long forced)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_COMPLETION, t);
	if (rec) {
		rec->data.completion.when   = sched_clock();
		rec->data.completion.forced = forced;
		put_record(rec);
	}
}

feather_callback void do_sched_trace_task_block(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_BLOCK, t);
	if (rec) {
		rec->data.block.when      = sched_clock();
		put_record(rec);
	}
}

feather_callback void do_sched_trace_task_resume(unsigned long id, unsigned long _task)
{
	struct task_struct *t = (struct task_struct*) _task;
	struct st_event_record* rec = get_record(ST_RESUME, t);
	if (rec) {
		rec->data.resume.when      = sched_clock();
		put_record(rec);
	}
}
