#ifndef NORQLOCK_H
#define NORQLOCK_H

typedef void (*work_t)(unsigned long arg);

struct no_rqlock_work {
	int			active;
	work_t			work;
	unsigned long		arg;
	struct no_rqlock_work*  next;
};

void init_no_rqlock_work(struct no_rqlock_work* w, work_t work,
			 unsigned long arg);

void __do_without_rqlock(struct no_rqlock_work *work);

static inline void do_without_rqlock(struct no_rqlock_work *work)
{
	if (!test_and_set_bit(0, (void*)&work->active))
		__do_without_rqlock(work);
}

void tick_no_rqlock(void);

#endif
