/* fdso.h - file descriptor attached shared objects
 *
 * (c) 2007 B. Brandenburg, LITMUS^RT project
 */

#ifndef _LINUX_FDSO_H_
#define _LINUX_FDSO_H_

#include <linux/list.h>
#include <asm/atomic.h>

#include <linux/fs.h>
#include <linux/slab.h>

#define MAX_OBJECT_DESCRIPTORS 85

typedef enum  {
	MIN_OBJ_TYPE 	= 0,

	FMLP_SEM	= 0,
	SRP_SEM		= 1,

	MPCP_SEM	= 2,
	MPCP_VS_SEM	= 3,
	DPCP_SEM	= 4,
	PCP_SEM         = 5,

	DFLP_SEM	= 6,

	MAX_OBJ_TYPE	= 6
} obj_type_t;

struct inode_obj_id {
	struct list_head	list;
	atomic_t		count;
	struct inode*		inode;

	obj_type_t 		type;
	void*			obj;
	unsigned int		id;
};

struct fdso_ops;

struct od_table_entry {
	unsigned int		used;

	struct inode_obj_id*	obj;
	const struct fdso_ops*	class;
};

struct fdso_ops {
	int   (*create)(void** obj_ref, obj_type_t type, void* __user);
	void  (*destroy)(obj_type_t type, void*);
	int   (*open)	(struct od_table_entry*, void* __user);
	int   (*close)	(struct od_table_entry*);
};

/* translate a userspace supplied od into the raw table entry
 * returns NULL if od is invalid
 */
struct od_table_entry* get_entry_for_od(int od);

/* translate a userspace supplied od into the associated object
 * returns NULL if od is invalid
 */
static inline void* od_lookup(int od, obj_type_t type)
{
	struct od_table_entry* e = get_entry_for_od(od);
	return e && e->obj->type == type ? e->obj->obj : NULL;
}

#define lookup_fmlp_sem(od)((struct pi_semaphore*)  od_lookup(od, FMLP_SEM))
#define lookup_srp_sem(od) ((struct srp_semaphore*) od_lookup(od, SRP_SEM))
#define lookup_ics(od)     ((struct ics*)           od_lookup(od, ICS_ID))


#endif
