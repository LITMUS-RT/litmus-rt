#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/debug_trace.h>
#include <litmus/reservations/reservation.h>

void reservation_init(struct reservation *res)
{
	memset(res, 0, sizeof(*res));
	res->state = RESERVATION_INACTIVE;
	INIT_LIST_HEAD(&res->clients);
	INIT_LIST_HEAD(&res->replenish_list);
	budget_notifier_list_init(&res->budget_notifiers);
}

struct task_struct* default_dispatch_client(
	struct reservation *res,
	lt_t *for_at_most)
{
	struct reservation_client *client, *next;
	struct task_struct* tsk;

	BUG_ON(res->state != RESERVATION_ACTIVE);
	*for_at_most = 0;

	list_for_each_entry_safe(client, next, &res->clients, list) {
		tsk = client->dispatch(client);
		if (likely(tsk)) {
			/* Primitive form of round-robin scheduling:
			 * make sure we alternate between multiple clients
			 * with at least the granularity of the replenishment
			 * period. Reservations that need more fine-grained
			 * or more predictable alternation between threads
			 * within a reservation should provide a custom
			 * dispatch function. */
			list_del(&client->list);
			/* move to back of list */
			list_add_tail(&client->list, &res->clients);
			return tsk;
		}
	}
	return NULL;
}

void common_drain_budget(
	struct reservation *res,
	lt_t how_much)
{
	if (how_much >= res->cur_budget)
		res->cur_budget = 0;
	else
		res->cur_budget -= how_much;

	res->budget_consumed += how_much;
	res->budget_consumed_total += how_much;

	switch (res->state) {
		case RESERVATION_DEPLETED:
		case RESERVATION_INACTIVE:
			BUG();
			break;

		case RESERVATION_ACTIVE_IDLE:
		case RESERVATION_ACTIVE:
			if (!res->cur_budget) {
				res->env->change_state(res->env, res,
					RESERVATION_DEPLETED);
			} /* else: stay in current state */
			break;
	}
}

static struct task_struct * task_client_dispatch(struct reservation_client *client)
{
	struct task_client *tc = container_of(client, struct task_client, client);
	return tc->task;
}

void task_client_init(struct task_client *tc, struct task_struct *tsk,
	struct reservation *res)
{
	memset(&tc->client, 0, sizeof(tc->client));
	tc->client.dispatch = task_client_dispatch;
	tc->client.reservation = res;
	tc->task = tsk;
}

static void sup_scheduler_update_at(
	struct sup_reservation_environment* sup_env,
	lt_t when)
{
	if (sup_env->next_scheduler_update > when)
		sup_env->next_scheduler_update = when;
}

static void sup_scheduler_update_after(
	struct sup_reservation_environment* sup_env,
	lt_t timeout)
{
	sup_scheduler_update_at(sup_env, sup_env->env.current_time + timeout);
}

static int _sup_queue_depleted(
	struct sup_reservation_environment* sup_env,
	struct reservation *res)
{
	struct list_head *pos;
	struct reservation *queued;
	int passed_earlier = 0;

	BUG_ON(in_list(&res->replenish_list));

	list_for_each(pos, &sup_env->depleted_reservations) {
		queued = list_entry(pos, struct reservation, replenish_list);
		if (queued->next_replenishment > res->next_replenishment) {
			list_add(&res->replenish_list, pos->prev);
			return passed_earlier;
		} else
			passed_earlier = 1;
	}

	list_add_tail(&res->replenish_list, &sup_env->depleted_reservations);

	return passed_earlier;
}

static void sup_queue_depleted(
	struct sup_reservation_environment* sup_env,
	struct reservation *res)
{
	int passed_earlier = _sup_queue_depleted(sup_env, res);

	/* check for updated replenishment time */
	if (!passed_earlier)
		sup_scheduler_update_at(sup_env, res->next_replenishment);
}

static int _sup_queue_active(
	struct sup_reservation_environment* sup_env,
	struct reservation *res)
{
	struct list_head *pos;
	struct reservation *queued;
	int passed_active = 0;

	if (likely(res->priority != RESERVATION_BACKGROUND_PRIORITY)) {
		/* enqueue in order of priority */
		list_for_each(pos, &sup_env->active_reservations) {
			queued = list_entry(pos, struct reservation, list);
			if (queued->priority > res->priority) {
				list_add(&res->list, pos->prev);
				return passed_active;
			} else if (queued->state == RESERVATION_ACTIVE)
				passed_active = 1;
		}
	} else {
		/* don't preempt unless the list happens to be empty */
		passed_active = !list_empty(&sup_env->active_reservations);
	}
	/* Either a background reservation, or we fell off the end of the list.
	 * In both cases, just add the reservation to the end of the list of
	 * active reservations. */
	list_add_tail(&res->list, &sup_env->active_reservations);
	return passed_active;
}

static void sup_queue_active(
	struct sup_reservation_environment* sup_env,
	struct reservation *res)
{
	int passed_active = _sup_queue_active(sup_env, res);

	/* check for possible preemption */
	if (res->state == RESERVATION_ACTIVE && !passed_active)
		sup_env->next_scheduler_update = SUP_RESCHEDULE_NOW;
	else if (res == list_first_entry(&sup_env->active_reservations,
	                                 struct reservation, list)) {
		/* First reservation is draining budget => make sure
		 * the scheduler is called to notice when the reservation
		 * budget has been drained completely. */
		sup_scheduler_update_after(sup_env, res->cur_budget);
	}
}

static void sup_queue_reservation(
	struct sup_reservation_environment* sup_env,
	struct reservation *res)
{
	switch (res->state) {
		case RESERVATION_INACTIVE:
			list_add(&res->list, &sup_env->inactive_reservations);
			break;

		case RESERVATION_DEPLETED:
			sup_queue_depleted(sup_env, res);
			break;

		case RESERVATION_ACTIVE_IDLE:
		case RESERVATION_ACTIVE:
			sup_queue_active(sup_env, res);
			break;
	}
}

void sup_add_new_reservation(
	struct sup_reservation_environment* sup_env,
	struct reservation* new_res)
{
	new_res->env = &sup_env->env;
	list_add(&new_res->all_list, &sup_env->all_reservations);
	sup_queue_reservation(sup_env, new_res);
}

struct reservation* sup_find_by_id(struct sup_reservation_environment* sup_env,
	unsigned int id)
{
	struct reservation *res;

	list_for_each_entry(res, &sup_env->all_reservations, all_list) {
		if (res->id == id)
			return res;
	}

	return NULL;
}

static void sup_charge_budget(
	struct sup_reservation_environment* sup_env,
	lt_t delta)
{
	struct reservation *res;

	/* charge the highest-priority ACTIVE or ACTIVE_IDLE reservation */

	res = list_first_entry_or_null(
		&sup_env->active_reservations, struct reservation, list);

	if (res) {
		TRACE("R%d: charging at %llu for %llu execution, budget before: %llu\n",
			res->id, res->env->current_time, delta, res->cur_budget);
		res->ops->drain_budget(res, delta);
		TRACE("R%d: budget now: %llu, priority: %llu\n",
			res->id, res->cur_budget, res->priority);
	}

	/* check when the next budget expires */

	res = list_first_entry_or_null(
		&sup_env->active_reservations, struct reservation, list);

	if (res) {
		/* make sure scheduler is invoked when this reservation expires
		 * its remaining budget */
		TRACE("requesting scheduler update for reservation %u "
			"in %llu nanoseconds\n",
			res->id, res->cur_budget);
		sup_scheduler_update_after(sup_env, res->cur_budget);
	}
}

static void sup_replenish_budgets(struct sup_reservation_environment* sup_env)
{
	struct list_head *pos, *next;
	struct reservation *res;

	list_for_each_safe(pos, next, &sup_env->depleted_reservations) {
		res = list_entry(pos, struct reservation, replenish_list);
		if (res->next_replenishment <= sup_env->env.current_time) {
			TRACE("R%d: replenishing budget at %llu, "
			      "priority: %llu\n",
				res->id, res->env->current_time, res->priority);
			res->ops->replenish(res);
		} else {
			/* list is ordered by increasing depletion times */
			break;
		}
	}

	/* request a scheduler update at the next replenishment instant */
	res = list_first_entry_or_null(&sup_env->depleted_reservations,
		struct reservation, replenish_list);
	if (res)
		sup_scheduler_update_at(sup_env, res->next_replenishment);
}

void sup_update_time(
	struct sup_reservation_environment* sup_env,
	lt_t now)
{
	lt_t delta;

	/* If the time didn't advance, there is nothing to do.
	 * This check makes it safe to call sup_advance_time() potentially
	 * multiple times (e.g., via different code paths. */
	if (!list_empty(&sup_env->active_reservations))
		TRACE("(sup_update_time) now: %llu, current_time: %llu\n", now,
			sup_env->env.current_time);
	if (unlikely(now <= sup_env->env.current_time))
		return;

	delta = now - sup_env->env.current_time;
	sup_env->env.current_time = now;

	/* check if future updates are required */
	if (sup_env->next_scheduler_update <= sup_env->env.current_time)
		sup_env->next_scheduler_update = SUP_NO_SCHEDULER_UPDATE;

	/* deplete budgets by passage of time */
	sup_charge_budget(sup_env, delta);

	/* check if any budgets were replenished */
	sup_replenish_budgets(sup_env);
}

struct task_struct* sup_dispatch(struct sup_reservation_environment* sup_env)
{
	struct reservation *res, *next;
	struct task_struct *tsk = NULL;
	lt_t time_slice;

	list_for_each_entry_safe(res, next, &sup_env->active_reservations, list) {
		if (res->state == RESERVATION_ACTIVE) {
			tsk = res->ops->dispatch_client(res, &time_slice);
			if (likely(tsk)) {
				if (time_slice)
				    sup_scheduler_update_after(sup_env, time_slice);
				sup_scheduler_update_after(sup_env, res->cur_budget);
				return tsk;
			}
		}
	}

	return NULL;
}

static void sup_res_change_state(
	struct reservation_environment* env,
	struct reservation *res,
	reservation_state_t new_state)
{
	struct sup_reservation_environment* sup_env;

	sup_env = container_of(env, struct sup_reservation_environment, env);

	TRACE("reservation R%d state %d->%d at %llu\n",
		res->id, res->state, new_state, env->current_time);

	if (new_state == RESERVATION_DEPLETED
	    && (res->state == RESERVATION_ACTIVE ||
	        res->state == RESERVATION_ACTIVE_IDLE)) {
		budget_notifiers_fire(&res->budget_notifiers, false);
	} else if (res->state == RESERVATION_DEPLETED
	           && new_state == RESERVATION_ACTIVE) {
		budget_notifiers_fire(&res->budget_notifiers, true);
	}

	/* dequeue prior to re-queuing */
	if (res->state == RESERVATION_DEPLETED)
		list_del(&res->replenish_list);
	else
		list_del(&res->list);

	/* check if we need to reschedule because we lost an active reservation */
	if (res->state == RESERVATION_ACTIVE && !sup_env->will_schedule)
		sup_env->next_scheduler_update = SUP_RESCHEDULE_NOW;
	res->state = new_state;
	sup_queue_reservation(sup_env, res);
}

static void sup_request_replenishment(
	struct reservation_environment* env,
	struct reservation *res)
{
	struct sup_reservation_environment* sup_env;

	sup_env = container_of(env, struct sup_reservation_environment, env);
	sup_queue_depleted(sup_env, res);
}

void sup_init(struct sup_reservation_environment* sup_env)
{
	memset(sup_env, 0, sizeof(*sup_env));

	INIT_LIST_HEAD(&sup_env->all_reservations);
	INIT_LIST_HEAD(&sup_env->active_reservations);
	INIT_LIST_HEAD(&sup_env->depleted_reservations);
	INIT_LIST_HEAD(&sup_env->inactive_reservations);

	sup_env->env.change_state = sup_res_change_state;
	sup_env->env.request_replenishment = sup_request_replenishment;

	sup_env->next_scheduler_update = SUP_NO_SCHEDULER_UPDATE;
}
