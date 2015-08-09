#include <litmus/reservations/budget-notifier.h>

void budget_notifier_list_init(struct budget_notifier_list* bnl)
{
	INIT_LIST_HEAD(&bnl->list);
	raw_spin_lock_init(&bnl->lock);
}

void budget_notifiers_fire(struct budget_notifier_list *bnl, bool replenished)
{
	struct budget_notifier *bn, *next;

	unsigned long flags;

	raw_spin_lock_irqsave(&bnl->lock, flags);

	list_for_each_entry_safe(bn, next, &bnl->list, list) {
		if (replenished)
			bn->budget_replenished(bn);
		else
			bn->budget_exhausted(bn);
	}

	raw_spin_unlock_irqrestore(&bnl->lock, flags);
}

