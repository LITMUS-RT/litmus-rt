/* fdso.c - file descriptor attached shared objects
 *
 * (c) 2007 B. Brandenburg, LITMUS^RT project
 *
 * Notes:
 *   - objects descriptor (OD) tables are not cloned during a fork.
 *   - objects are created on-demand, and freed after the last reference
 *     is dropped.
 *   - for now, object types are hard coded.
 *   - As long as we have live objects, we keep a reference to the inode.
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/file.h>
#include <asm/uaccess.h>

#include <litmus/fdso.h>

extern struct fdso_ops generic_lock_ops;

static const struct fdso_ops* fdso_ops[] = {
	&generic_lock_ops, /* FMLP_SEM */
	&generic_lock_ops, /* SRP_SEM */
	&generic_lock_ops, /* MPCP_SEM */
	&generic_lock_ops, /* MPCP_VS_SEM */
	&generic_lock_ops, /* DPCP_SEM */
	&generic_lock_ops, /* PCP_SEM */
	&generic_lock_ops, /* DFLP_SEM */
};

static int fdso_create(void** obj_ref, obj_type_t type, void* __user config)
{
	BUILD_BUG_ON(ARRAY_SIZE(fdso_ops) != MAX_OBJ_TYPE + 1);

	if (fdso_ops[type]->create)
		return fdso_ops[type]->create(obj_ref, type, config);
	else
		return -EINVAL;
}

static void fdso_destroy(obj_type_t type, void* obj)
{
	fdso_ops[type]->destroy(type, obj);
}

static int fdso_open(struct od_table_entry* entry, void* __user config)
{
	if (fdso_ops[entry->obj->type]->open)
		return fdso_ops[entry->obj->type]->open(entry, config);
	else
		return 0;
}

static int fdso_close(struct od_table_entry* entry)
{
	if (fdso_ops[entry->obj->type]->close)
		return fdso_ops[entry->obj->type]->close(entry);
	else
		return 0;
}

/* inode must be locked already */
static int alloc_inode_obj(struct inode_obj_id** obj_ref,
			   struct inode* inode,
			   obj_type_t type,
			   unsigned int id,
			   void* __user config)
{
	struct inode_obj_id* obj;
	void* raw_obj;
	int err;

	obj = kmalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		return -ENOMEM;
	}

	err = fdso_create(&raw_obj, type, config);
	if (err != 0) {
		kfree(obj);
		return err;
	}

	INIT_LIST_HEAD(&obj->list);
	atomic_set(&obj->count, 1);
	obj->type  = type;
	obj->id    = id;
	obj->obj   = raw_obj;
	obj->inode = inode;

	list_add(&obj->list, &inode->i_obj_list);
	atomic_inc(&inode->i_count);

	printk(KERN_DEBUG "alloc_inode_obj(%p, %d, %d): object created\n", inode, type, id);

	*obj_ref = obj;
	return 0;
}

/* inode must be locked already */
static struct inode_obj_id* get_inode_obj(struct inode* inode,
					  obj_type_t type,
					  unsigned int id)
{
	struct list_head* pos;
	struct inode_obj_id* obj = NULL;

	list_for_each(pos, &inode->i_obj_list) {
		obj = list_entry(pos, struct inode_obj_id, list);
		if (obj->id == id && obj->type == type) {
			atomic_inc(&obj->count);
			return obj;
		}
	}
	printk(KERN_DEBUG "get_inode_obj(%p, %d, %d): couldn't find object\n", inode, type, id);
	return NULL;
}


static void put_inode_obj(struct inode_obj_id* obj)
{
	struct inode* inode;
	int let_go = 0;

	inode = obj->inode;
	if (atomic_dec_and_test(&obj->count)) {

		mutex_lock(&inode->i_obj_mutex);
		/* no new references can be obtained */
		if (!atomic_read(&obj->count)) {
			list_del(&obj->list);
			fdso_destroy(obj->type, obj->obj);
			kfree(obj);
			let_go = 1;
		}
		mutex_unlock(&inode->i_obj_mutex);
		if (let_go)
			iput(inode);
	}
}

static struct od_table_entry*  get_od_entry(struct task_struct* t)
{
	struct od_table_entry* table;
	int i;


	table = t->od_table;
	if (!table) {
		table = kzalloc(sizeof(*table) * MAX_OBJECT_DESCRIPTORS,
				GFP_KERNEL);
		t->od_table = table;
	}

	for (i = 0; table &&  i < MAX_OBJECT_DESCRIPTORS; i++)
		if (!table[i].used) {
			table[i].used = 1;
			return table + i;
		}
	return NULL;
}

static int put_od_entry(struct od_table_entry* od)
{
	put_inode_obj(od->obj);
	od->used = 0;
	return 0;
}

static long close_od_entry(struct od_table_entry *od)
{
	long ret;

	/* Give the class a chance to reject the close. */
	ret = fdso_close(od);
	if (ret == 0)
		ret = put_od_entry(od);

	return ret;
}

void exit_od_table(struct task_struct* t)
{
	int i;

	if (t->od_table) {
		for (i = 0; i < MAX_OBJECT_DESCRIPTORS; i++)
			if (t->od_table[i].used)
				close_od_entry(t->od_table + i);
		kfree(t->od_table);
		t->od_table = NULL;
	}
}

static int do_sys_od_open(struct file* file, obj_type_t type, int id,
			  void* __user config)
{
	int idx = 0, err = 0;
	struct inode* inode;
	struct inode_obj_id* obj = NULL;
	struct od_table_entry* entry;

	inode = file->f_dentry->d_inode;

	entry = get_od_entry(current);
	if (!entry)
		return -ENOMEM;

	mutex_lock(&inode->i_obj_mutex);
	obj = get_inode_obj(inode, type, id);
	if (!obj)
		err = alloc_inode_obj(&obj, inode, type, id, config);
	if (err != 0) {
		obj = NULL;
		idx = err;
		entry->used = 0;
	} else {
		entry->obj   = obj;
		entry->class = fdso_ops[type];
		idx = entry - current->od_table;
	}

	mutex_unlock(&inode->i_obj_mutex);

	/* open only if creation succeeded */
	if (!err)
		err = fdso_open(entry, config);
	if (err < 0) {
		/* The class rejected the open call.
		 * We need to clean up and tell user space.
		 */
		if (obj)
			put_od_entry(entry);
		idx = err;
	}

	return idx;
}


struct od_table_entry* get_entry_for_od(int od)
{
	struct task_struct *t = current;

	if (!t->od_table)
		return NULL;
	if (od < 0 || od >= MAX_OBJECT_DESCRIPTORS)
		return NULL;
	if (!t->od_table[od].used)
		return NULL;
	return t->od_table + od;
}


asmlinkage long sys_od_open(int fd, int type, int obj_id, void* __user config)
{
	int ret = 0;
	struct file*  file;

	/*
	   1) get file from fd, get inode from file
	   2) lock inode
	   3) try to lookup object
	   4) if not present create and enqueue object, inc inode refcnt
	   5) increment refcnt of object
	   6) alloc od_table_entry, setup ptrs
	   7) unlock inode
	   8) return offset in od_table as OD
	 */

	if (type < MIN_OBJ_TYPE || type > MAX_OBJ_TYPE) {
		ret = -EINVAL;
		goto out;
	}

	file = fget(fd);
	if (!file) {
		ret = -EBADF;
		goto out;
	}

	ret = do_sys_od_open(file, type, obj_id, config);

	fput(file);

out:
	return ret;
}


asmlinkage long sys_od_close(int od)
{
	int ret = -EINVAL;
	struct task_struct *t = current;

	if (od < 0 || od >= MAX_OBJECT_DESCRIPTORS)
		return ret;

	if (!t->od_table || !t->od_table[od].used)
		return ret;


	ret = close_od_entry(t->od_table + od);

	return ret;
}
