#ifndef LITMUS_BUDGET_NOTIFIER_H
#define LITMUS_BUDGET_NOTIFIER_H

#include <linux/list.h>
#include <linux/spinlock.h>

struct budget_notifier;

typedef void (*budget_callback_t)  (
	struct budget_notifier *bn
);

struct budget_notifier {
	struct list_head list;
	budget_callback_t budget_exhausted;
	budget_callback_t budget_replenished;
};

struct budget_notifier_list {
	struct list_head list;
	raw_spinlock_t lock;
};

void budget_notifier_list_init(struct budget_notifier_list* bnl);

static inline void budget_notifier_add(
	struct budget_notifier_list *bnl,
	struct budget_notifier *bn)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&bnl->lock, flags);
	list_add(&bn->list, &bnl->list);
	raw_spin_unlock_irqrestore(&bnl->lock, flags);
}

static inline void budget_notifier_remove(
	struct budget_notifier_list *bnl,
	struct budget_notifier *bn)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&bnl->lock, flags);
	list_del(&bn->list);
	raw_spin_unlock_irqrestore(&bnl->lock, flags);
}

void budget_notifiers_fire(struct budget_notifier_list *bnl, bool replenished);

#endif
