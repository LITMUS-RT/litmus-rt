/* fdso.h - file descriptor attached shared objects
 *
 * (c) 2007 B. Brandenburg, LITMUS^RT project
 */

#ifndef _LINUX_FDSO_H_
#define _LINUX_FDSO_H_

#include <linux/list.h>
#include <asm/atomic.h>

#include <linux/fs.h>

#define MAX_OBJECT_DESCRIPTORS 32

typedef enum  {
	MIN_OBJ_TYPE 	= 0,

	PI_SEM 		= 0,
	SRP_SEM		= 1,

	MAX_OBJ_TYPE	= 1
} obj_type_t;

struct inode_obj_id {
	struct list_head	list;
	atomic_t		count;
	struct inode*		inode;

	obj_type_t 		type;
	void*			obj;
	unsigned int		id;
};


struct od_table_entry {
	unsigned int		used;

	struct inode_obj_id*	obj;
	void*			extra;
};

struct fdso_ops {
	void* (*create)	(void);
	void  (*destroy)(void*);
	int   (*open)	(struct od_table_entry*, void* __user);
	int   (*close)	(struct od_table_entry*);
};

/* translate a userspace supplied od into the raw table entry
 * returns NULL if od is invalid
 */
struct od_table_entry* __od_lookup(int od);

/* translate a userspace supplied od into the associated object
 * returns NULL if od is invalid
 */
static inline void* od_lookup(int od, obj_type_t type)
{
	struct od_table_entry* e = __od_lookup(od);
	return e && e->obj->type == type ? e->obj->obj : NULL;
}

#define lookup_pi_sem(od)  ((struct pi_semaphore*)  od_lookup(od, PI_SEM))
#define lookup_srp_sem(od) ((struct srp_semaphore*) od_lookup(od, SRP_SEM))
#define lookup_ics(od)     ((struct ics*)           od_lookup(od, ICS_ID))


#endif
