#ifndef LITMUS_RESERVATIONS_ALLOC_H
#define LITMUS_RESERVATIONS_ALLOC_H

#include <litmus/reservations/reservation.h>

long alloc_polling_reservation(
	int res_type,
	struct reservation_config *config,
	struct reservation **_res);

long alloc_table_driven_reservation(
	struct reservation_config *config,
	struct reservation **_res);

#endif