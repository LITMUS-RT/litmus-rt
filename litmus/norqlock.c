#include <linux/list.h>
#include <linux/bitops.h>
#include <linux/percpu.h>
#include <linux/module.h>
#include <linux/smp.h>

#include <litmus/norqlock.h>

struct worklist {
	struct no_rqlock_work* next;
};

static DEFINE_PER_CPU(struct worklist, norq_worklist) = {NULL};

void init_no_rqlock_work(struct no_rqlock_work* w, work_t work,
			 unsigned long arg)
{
	w->active = 0;
	w->work   = work;
	w->arg    = arg;
	w->next   = NULL;
}

void __do_without_rqlock(struct no_rqlock_work *work)
{
	long flags;
	struct worklist* wl;

	local_irq_save(flags);
	wl = &__get_cpu_var(norq_worklist);
	work->next = wl->next;
	wl->next   = work;
	local_irq_restore(flags);
}

void tick_no_rqlock(void)
{
	long flags;
	struct no_rqlock_work *todo, *next;

	local_irq_save(flags);

	next = __get_cpu_var(norq_worklist).next;
	__get_cpu_var(norq_worklist).next = NULL;

	local_irq_restore(flags);

	while (next) {
		todo = next;
		next = next->next;
		todo->next = NULL;
		smp_mb__before_clear_bit();
		clear_bit(0, (void*) &todo->active);
		todo->work(todo->arg);
	}


}
