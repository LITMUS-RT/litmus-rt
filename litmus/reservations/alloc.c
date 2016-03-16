#include <linux/slab.h>
#include <asm/uaccess.h>

#include <litmus/rt_param.h>

#include <litmus/reservations/alloc.h>
#include <litmus/reservations/polling.h>
#include <litmus/reservations/table-driven.h>


long alloc_polling_reservation(
	int res_type,
	struct reservation_config *config,
	struct reservation **_res)
{
	struct polling_reservation *pres;
	int use_edf  = config->priority == LITMUS_NO_PRIORITY;
	int periodic =  res_type == PERIODIC_POLLING;

	if (config->polling_params.budget >
	    config->polling_params.period) {
		printk(KERN_ERR "invalid polling reservation (%u): "
		       "budget > period\n", config->id);
		return -EINVAL;
	}
	if (config->polling_params.budget >
	    config->polling_params.relative_deadline
	    && config->polling_params.relative_deadline) {
		printk(KERN_ERR "invalid polling reservation (%u): "
		       "budget > deadline\n", config->id);
		return -EINVAL;
	}
	if (config->polling_params.offset >
	    config->polling_params.period) {
		printk(KERN_ERR "invalid polling reservation (%u): "
		       "offset > period\n", config->id);
		return -EINVAL;
	}

	/* XXX: would be nice to use a core-local allocation. */
	pres = kzalloc(sizeof(*pres), GFP_KERNEL);
	if (!pres)
		return -ENOMEM;

	polling_reservation_init(pres, use_edf, periodic,
		config->polling_params.budget,
		config->polling_params.period,
		config->polling_params.relative_deadline,
		config->polling_params.offset);
	pres->res.id = config->id;
	if (!use_edf)
		pres->res.priority = config->priority;

	*_res = &pres->res;
	return 0;
}


#define MAX_INTERVALS 1024

long alloc_table_driven_reservation(
	struct reservation_config *config,
	struct reservation **_res)
{
	struct table_driven_reservation *td_res = NULL;
	struct lt_interval *slots = NULL;
	size_t slots_size;
	unsigned int i, num_slots;
	long err = -EINVAL;
	void *mem;

	if (!config->table_driven_params.num_intervals) {
		printk(KERN_ERR "invalid table-driven reservation (%u): "
		       "no intervals\n", config->id);
		return -EINVAL;
	}

	if (config->table_driven_params.num_intervals > MAX_INTERVALS) {
		printk(KERN_ERR "invalid table-driven reservation (%u): "
		       "too many intervals (max: %d)\n", config->id, MAX_INTERVALS);
		return -EINVAL;
	}

	num_slots = config->table_driven_params.num_intervals;
	slots_size = sizeof(slots[0]) * num_slots;

	mem = kzalloc(sizeof(*td_res) + slots_size, GFP_KERNEL);
	if (!mem) {
		return -ENOMEM;
	} else {
		slots  = mem + sizeof(*td_res);
		td_res = mem;
		err = copy_from_user(slots,
			config->table_driven_params.intervals, slots_size);
	}

	if (!err) {
		/* sanity checks */
		for (i = 0; !err && i < num_slots; i++)
			if (slots[i].end <= slots[i].start) {
				printk(KERN_ERR
				       "invalid table-driven reservation (%u): "
				       "invalid interval %u => [%llu, %llu]\n",
				       config->id, i,
				       slots[i].start, slots[i].end);
				err = -EINVAL;
			}

		for (i = 0; !err && i + 1 < num_slots; i++)
			if (slots[i + 1].start <= slots[i].end) {
				printk(KERN_ERR
				       "invalid table-driven reservation (%u): "
				       "overlapping intervals %u, %u\n",
				       config->id, i, i + 1);
				err = -EINVAL;
			}

		if (slots[num_slots - 1].end >
			config->table_driven_params.major_cycle_length) {
			printk(KERN_ERR
				"invalid table-driven reservation (%u): last "
				"interval ends past major cycle %llu > %llu\n",
				config->id,
				slots[num_slots - 1].end,
				config->table_driven_params.major_cycle_length);
			err = -EINVAL;
		}
	}

	if (err) {
		kfree(td_res);
	} else {
		table_driven_reservation_init(td_res,
			config->table_driven_params.major_cycle_length,
			slots, num_slots);
		td_res->res.id = config->id;
		td_res->res.priority = config->priority;
		*_res = &td_res->res;
	}

	return err;
}

