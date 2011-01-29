#include <litmus/fdso.h>

#ifdef CONFIG_LITMUS_LOCKING

#include <litmus/sched_plugin.h>
#include <litmus/trace.h>

static void* create_generic_lock(obj_type_t type);
static int open_generic_lock(struct od_table_entry* entry, void* __user arg);
static int close_generic_lock(struct od_table_entry* entry);
static void destroy_generic_lock(obj_type_t type, void* sem);

struct fdso_ops generic_lock_ops = {
	.create  = create_generic_lock,
	.open    = open_generic_lock,
	.close   = close_generic_lock,
	.destroy = destroy_generic_lock
};

static inline bool is_lock(struct od_table_entry* entry)
{
	return entry->class == &generic_lock_ops;
}

static inline struct litmus_lock* get_lock(struct od_table_entry* entry)
{
	BUG_ON(!is_lock(entry));
	return (struct litmus_lock*) entry->obj->obj;
}

static  void* create_generic_lock(obj_type_t type)
{
	struct litmus_lock* lock;
	int err;

	err = litmus->allocate_lock(&lock, type);
	if (err == 0)
		return lock;
	else
		return NULL;
}

static int open_generic_lock(struct od_table_entry* entry, void* __user arg)
{
	struct litmus_lock* lock = get_lock(entry);
	if (lock->ops->open)
		return lock->ops->open(lock, arg);
	else
		return 0; /* default: any task can open it */
}

static int close_generic_lock(struct od_table_entry* entry)
{
	struct litmus_lock* lock = get_lock(entry);
	if (lock->ops->close)
		return lock->ops->close(lock);
	else
		return 0; /* default: closing succeeds */
}

static void destroy_generic_lock(obj_type_t type, void* obj)
{
	struct litmus_lock* lock = (struct litmus_lock*) obj;
	lock->ops->deallocate(lock);
}

asmlinkage long sys_litmus_lock(int lock_od)
{
	long err = -EINVAL;
	struct od_table_entry* entry;
	struct litmus_lock* l;

	TS_PI_DOWN_START;

	entry = get_entry_for_od(lock_od);
	if (entry && is_lock(entry)) {
		l = get_lock(entry);
		TRACE_CUR("attempts to lock 0x%p\n", l);
		err = l->ops->lock(l);
	}

	/* Note: task my have been suspended or preempted in between!  Take
	 * this into account when computing overheads. */
	TS_PI_DOWN_END;

	return err;
}

asmlinkage long sys_litmus_unlock(int lock_od)
{
	long err = -EINVAL;
	struct od_table_entry* entry;
	struct litmus_lock* l;

	TS_PI_UP_START;

	entry = get_entry_for_od(lock_od);
	if (entry && is_lock(entry)) {
		l = get_lock(entry);
		TRACE_CUR("attempts to unlock 0x%p\n", l);
		err = l->ops->unlock(l);
	}

	/* Note: task my have been preempted in between!  Take this into
	 * account when computing overheads. */
	TS_PI_UP_END;

	return err;
}

struct task_struct* waitqueue_first(wait_queue_head_t *wq)
{
	wait_queue_t *q;

	if (waitqueue_active(wq)) {
		q = list_entry(wq->task_list.next,
			       wait_queue_t, task_list);
		return (struct task_struct*) q->private;
	} else
		return NULL;
}


#else

struct fdso_ops generic_lock_ops = {};

asmlinkage long sys_litmus_lock(int sem_od)
{
	return -ENOSYS;
}

asmlinkage long sys_litmus_unlock(int sem_od)
{
	return -ENOSYS;
}

#endif
