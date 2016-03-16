#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/reservations/reservation.h>
#include <litmus/reservations/table-driven.h>

static lt_t td_cur_major_cycle_start(struct table_driven_reservation *tdres)
{
	lt_t x, tmp;

	tmp = tdres->res.env->current_time - tdres->res.env->time_zero;
	x = div64_u64(tmp, tdres->major_cycle);
	x *= tdres->major_cycle;
	return x;
}


static lt_t td_next_major_cycle_start(struct table_driven_reservation *tdres)
{
	lt_t x, tmp;

	tmp = tdres->res.env->current_time - tdres->res.env->time_zero;
	x = div64_u64(tmp, tdres->major_cycle) + 1;
	x *= tdres->major_cycle;
	return x;
}

static void td_client_arrives(
	struct reservation* res,
	struct reservation_client *client
)
{
	struct table_driven_reservation *tdres =
		container_of(res, struct table_driven_reservation, res);

	list_add_tail(&client->list, &res->clients);

	switch (res->state) {
		case RESERVATION_INACTIVE:
			/* Figure out first replenishment time. */
			tdres->major_cycle_start = td_next_major_cycle_start(tdres);
			res->next_replenishment  = tdres->major_cycle_start;
			res->next_replenishment += tdres->intervals[0].start;
			tdres->next_interval = 0;

			res->env->change_state(res->env, res,
				RESERVATION_DEPLETED);
			break;

		case RESERVATION_ACTIVE:
		case RESERVATION_DEPLETED:
			/* do nothing */
			break;

		case RESERVATION_ACTIVE_IDLE:
			res->env->change_state(res->env, res,
				RESERVATION_ACTIVE);
			break;
	}
}

static void td_client_departs(
	struct reservation *res,
	struct reservation_client *client,
	int did_signal_job_completion
)
{
	list_del(&client->list);

	switch (res->state) {
		case RESERVATION_INACTIVE:
		case RESERVATION_ACTIVE_IDLE:
			BUG(); /* INACTIVE or IDLE <=> no client */
			break;

		case RESERVATION_ACTIVE:
			if (list_empty(&res->clients)) {
				res->env->change_state(res->env, res,
						RESERVATION_ACTIVE_IDLE);
			} /* else: nothing to do, more clients ready */
			break;

		case RESERVATION_DEPLETED:
			/* do nothing */
			break;
	}
}

static lt_t td_time_remaining_until_end(struct table_driven_reservation *tdres)
{
	lt_t now = tdres->res.env->current_time;
	lt_t end = tdres->cur_interval.end;
	TRACE("td_remaining(%u): start=%llu now=%llu end=%llu state=%d\n",
		tdres->res.id,
		tdres->cur_interval.start,
		now, end,
		tdres->res.state);
	if (now >=  end)
		return 0;
	else
		return end - now;
}

static void td_replenish(
	struct reservation *res)
{
	struct table_driven_reservation *tdres =
		container_of(res, struct table_driven_reservation, res);

	TRACE("td_replenish(%u): expected_replenishment=%llu\n", res->id,
		res->next_replenishment);

	/* figure out current interval */
	tdres->cur_interval.start = tdres->major_cycle_start +
		tdres->intervals[tdres->next_interval].start;
	tdres->cur_interval.end =  tdres->major_cycle_start +
		tdres->intervals[tdres->next_interval].end;
	TRACE("major_cycle_start=%llu => [%llu, %llu]\n",
		tdres->major_cycle_start,
		tdres->cur_interval.start,
		tdres->cur_interval.end);

	/* reset budget */
	res->cur_budget = td_time_remaining_until_end(tdres);
	res->budget_consumed = 0;
	TRACE("td_replenish(%u): %s budget=%llu\n", res->id,
		res->cur_budget ? "" : "WARNING", res->cur_budget);

	/* prepare next slot */
	tdres->next_interval = (tdres->next_interval + 1) % tdres->num_intervals;
	if (!tdres->next_interval)
		/* wrap to next major cycle */
		tdres->major_cycle_start += tdres->major_cycle;

	/* determine next time this reservation becomes eligible to execute */
	res->next_replenishment  = tdres->major_cycle_start;
	res->next_replenishment += tdres->intervals[tdres->next_interval].start;
	TRACE("td_replenish(%u): next_replenishment=%llu\n", res->id,
		res->next_replenishment);


	switch (res->state) {
		case RESERVATION_DEPLETED:
		case RESERVATION_ACTIVE:
		case RESERVATION_ACTIVE_IDLE:
			if (list_empty(&res->clients))
				res->env->change_state(res->env, res,
					RESERVATION_ACTIVE_IDLE);
			else
				/* we have clients & budget => ACTIVE */
				res->env->change_state(res->env, res,
					RESERVATION_ACTIVE);
			break;

		case RESERVATION_INACTIVE:
			BUG();
			break;
	}
}

static void td_drain_budget(
		struct reservation *res,
		lt_t how_much)
{
	struct table_driven_reservation *tdres =
		container_of(res, struct table_driven_reservation, res);

	res->budget_consumed += how_much;
	res->budget_consumed_total += how_much;

	/* Table-driven scheduling: instead of tracking the budget, we compute
	 * how much time is left in this allocation interval. */

	/* sanity check: we should never try to drain from future slots */
	BUG_ON(tdres->cur_interval.start > res->env->current_time);

	switch (res->state) {
		case RESERVATION_DEPLETED:
		case RESERVATION_INACTIVE:
			BUG();
			break;

		case RESERVATION_ACTIVE_IDLE:
		case RESERVATION_ACTIVE:
			res->cur_budget = td_time_remaining_until_end(tdres);
			TRACE("td_drain_budget(%u): drained to budget=%llu\n",
				res->id, res->cur_budget);
			if (!res->cur_budget) {
				res->env->change_state(res->env, res,
					RESERVATION_DEPLETED);
			} else {
				/* sanity check budget calculation */
				BUG_ON(res->env->current_time >= tdres->cur_interval.end);
				BUG_ON(res->env->current_time < tdres->cur_interval.start);
			}

			break;
	}
}

static struct task_struct* td_dispatch_client(
	struct reservation *res,
	lt_t *for_at_most)
{
	struct task_struct *t;
	struct table_driven_reservation *tdres =
		container_of(res, struct table_driven_reservation, res);

	/* usual logic for selecting a client */
	t = default_dispatch_client(res, for_at_most);

	TRACE_TASK(t, "td_dispatch_client(%u): selected, budget=%llu\n",
		res->id, res->cur_budget);

	/* check how much budget we have left in this time slot */
	res->cur_budget = td_time_remaining_until_end(tdres);

	TRACE_TASK(t, "td_dispatch_client(%u): updated to budget=%llu next=%d\n",
		res->id, res->cur_budget, tdres->next_interval);

	if (unlikely(!res->cur_budget)) {
		/* Unlikely case: if we ran out of budget, the user configured
		 * a broken scheduling table (overlapping table slots).
		 * Not much we can do about this, but we can't dispatch a job
		 * now without causing overload. So let's register this reservation
		 * as depleted and wait for the next allocation. */
		TRACE("td_dispatch_client(%u): budget unexpectedly depleted "
			"(check scheduling table for unintended overlap)\n",
			res->id);
		res->env->change_state(res->env, res,
			RESERVATION_DEPLETED);
		return NULL;
	} else
		return t;
}

static struct reservation_ops td_ops = {
	.dispatch_client = td_dispatch_client,
	.client_arrives = td_client_arrives,
	.client_departs = td_client_departs,
	.replenish = td_replenish,
	.drain_budget = td_drain_budget,
};

void table_driven_reservation_init(
	struct table_driven_reservation *tdres,
	lt_t major_cycle,
	struct lt_interval *intervals,
	unsigned int num_intervals)
{
	unsigned int i;

	/* sanity checking */
	BUG_ON(!num_intervals);
	for (i = 0; i < num_intervals; i++)
		BUG_ON(intervals[i].end <= intervals[i].start);
	for (i = 0; i + 1 < num_intervals; i++)
		BUG_ON(intervals[i + 1].start <= intervals[i].end);
	BUG_ON(intervals[num_intervals - 1].end > major_cycle);

	reservation_init(&tdres->res);
	tdres->res.kind = TABLE_DRIVEN;
	tdres->major_cycle = major_cycle;
	tdres->intervals = intervals;
	tdres->cur_interval.start = 0;
	tdres->cur_interval.end   = 0;
	tdres->num_intervals = num_intervals;
	tdres->res.ops = &td_ops;
}
