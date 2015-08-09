#ifndef LITMUS_POLLING_RESERVATIONS_H
#define LITMUS_POLLING_RESERVATIONS_H

#include <litmus/reservations/reservation.h>

struct polling_reservation {
	/* extend basic reservation */
	struct reservation res;

	lt_t max_budget;
	lt_t period;
	lt_t deadline;
	lt_t offset;
};

void polling_reservation_init(struct polling_reservation *pres, int use_edf_prio,
	int use_periodic_polling, lt_t budget, lt_t period, lt_t deadline, lt_t offset);

#endif
