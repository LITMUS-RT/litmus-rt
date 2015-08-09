#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/reservations/reservation.h>
#include <litmus/reservations/polling.h>


static void periodic_polling_client_arrives(
	struct reservation* res,
	struct reservation_client *client
)
{
	struct polling_reservation *pres =
		container_of(res, struct polling_reservation, res);
	lt_t instances, tmp;

	list_add_tail(&client->list, &res->clients);

	switch (res->state) {
		case RESERVATION_INACTIVE:
			/* Figure out next replenishment time. */
			tmp = res->env->current_time - res->env->time_zero;
			instances =  div64_u64(tmp, pres->period);
			res->next_replenishment =
				(instances + 1) * pres->period + pres->offset;

			TRACE("pol-res: activate tmp=%llu instances=%llu period=%llu nextrp=%llu cur=%llu\n",
				tmp, instances, pres->period, res->next_replenishment,
				res->env->current_time);

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


static void periodic_polling_client_departs(
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

static void periodic_polling_on_replenishment(
	struct reservation *res
)
{
	struct polling_reservation *pres =
		container_of(res, struct polling_reservation, res);

	/* replenish budget */
	res->cur_budget = pres->max_budget;
	res->next_replenishment += pres->period;
	res->budget_consumed = 0;

	switch (res->state) {
		case RESERVATION_DEPLETED:
		case RESERVATION_INACTIVE:
		case RESERVATION_ACTIVE_IDLE:
			if (list_empty(&res->clients))
				/* no clients => poll again later */
				res->env->change_state(res->env, res,
					RESERVATION_INACTIVE);
			else
				/* we have clients & budget => ACTIVE */
				res->env->change_state(res->env, res,
					RESERVATION_ACTIVE);
			break;

		case RESERVATION_ACTIVE:
			/* Replenished while active => tardy? In any case,
			 * go ahead and stay active. */
			break;
	}
}

static void periodic_polling_on_replenishment_edf(
	struct reservation *res
)
{
	struct polling_reservation *pres =
		container_of(res, struct polling_reservation, res);

	/* update current priority */
	res->priority = res->next_replenishment + pres->deadline;

	/* do common updates */
	periodic_polling_on_replenishment(res);
}

static struct reservation_ops periodic_polling_ops_fp = {
	.dispatch_client = default_dispatch_client,
	.client_arrives = periodic_polling_client_arrives,
	.client_departs = periodic_polling_client_departs,
	.replenish = periodic_polling_on_replenishment,
	.drain_budget = common_drain_budget,
};

static struct reservation_ops periodic_polling_ops_edf = {
	.dispatch_client = default_dispatch_client,
	.client_arrives = periodic_polling_client_arrives,
	.client_departs = periodic_polling_client_departs,
	.replenish = periodic_polling_on_replenishment_edf,
	.drain_budget = common_drain_budget,
};




static void sporadic_polling_client_arrives_fp(
	struct reservation* res,
	struct reservation_client *client
)
{
	struct polling_reservation *pres =
		container_of(res, struct polling_reservation, res);

	list_add_tail(&client->list, &res->clients);

	switch (res->state) {
		case RESERVATION_INACTIVE:
			/* Replenish now. */
			res->cur_budget = pres->max_budget;
			res->next_replenishment =
				res->env->current_time + pres->period;

			res->env->change_state(res->env, res,
				RESERVATION_ACTIVE);
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

static void sporadic_polling_client_arrives_edf(
	struct reservation* res,
	struct reservation_client *client
)
{
	struct polling_reservation *pres =
		container_of(res, struct polling_reservation, res);

	list_add_tail(&client->list, &res->clients);

	switch (res->state) {
		case RESERVATION_INACTIVE:
			/* Replenish now. */
			res->cur_budget = pres->max_budget;
			res->next_replenishment =
				res->env->current_time + pres->period;
			res->priority =
				res->env->current_time + pres->deadline;

			res->env->change_state(res->env, res,
				RESERVATION_ACTIVE);
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

static struct reservation_ops sporadic_polling_ops_fp = {
	.dispatch_client = default_dispatch_client,
	.client_arrives = sporadic_polling_client_arrives_fp,
	.client_departs = periodic_polling_client_departs,
	.replenish = periodic_polling_on_replenishment,
	.drain_budget = common_drain_budget,
};

static struct reservation_ops sporadic_polling_ops_edf = {
	.dispatch_client = default_dispatch_client,
	.client_arrives = sporadic_polling_client_arrives_edf,
	.client_departs = periodic_polling_client_departs,
	.replenish = periodic_polling_on_replenishment_edf,
	.drain_budget = common_drain_budget,
};

void polling_reservation_init(
	struct polling_reservation *pres,
	int use_edf_prio,
	int use_periodic_polling,
	lt_t budget, lt_t period, lt_t deadline, lt_t offset
)
{
	if (!deadline)
		deadline = period;
	BUG_ON(budget > period);
	BUG_ON(budget > deadline);
	BUG_ON(offset >= period);

	reservation_init(&pres->res);
	pres->max_budget = budget;
	pres->period = period;
	pres->deadline = deadline;
	pres->offset = offset;
	if (use_periodic_polling) {
		pres->res.kind = PERIODIC_POLLING;
		if (use_edf_prio)
			pres->res.ops = &periodic_polling_ops_edf;
		else
			pres->res.ops = &periodic_polling_ops_fp;
	} else {
		pres->res.kind = SPORADIC_POLLING;
		if (use_edf_prio)
			pres->res.ops = &sporadic_polling_ops_edf;
		else
			pres->res.ops = &sporadic_polling_ops_fp;
	}
}
