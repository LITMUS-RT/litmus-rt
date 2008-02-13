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

extern struct fdso_ops pi_sem_ops;
extern struct fdso_ops srp_sem_ops;

static const struct fdso_ops* fdso_ops[] = {
	&pi_sem_ops,
	&srp_sem_ops,
};

static void* fdso_create(obj_type_t type)
{
	return fdso_ops[type]->create();
}

static void fdso_destroy(obj_type_t type, void* obj)
{
	fdso_ops[type]->destroy(obj);
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
static struct inode_obj_id* alloc_inode_obj(struct inode* inode,
					    obj_type_t type,
					    unsigned int id)
{
	struct inode_obj_id* obj;
	void* raw_obj;

	raw_obj = fdso_create(type);
	if (!raw_obj)
		return NULL;

	obj = kmalloc(sizeof(struct inode_obj_id), GFP_KERNEL);
	if (!obj)
		return NULL;
	INIT_LIST_HEAD(&obj->list);
	atomic_set(&obj->count, 1);
	obj->type  = type;
	obj->id    = id;
	obj->obj   = raw_obj;
	obj->inode = inode;

	list_add(&obj->list, &inode->i_obj_list);
	atomic_inc(&inode->i_count);

	printk(KERN_DEBUG "alloc_inode_obj(%p, %d, %d): object created\n", inode, type, id);
	return obj;
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
		table = (struct od_table_entry*)
			kzalloc(sizeof(struct  od_table_entry) *
				MAX_OBJECT_DESCRIPTORS, GFP_KERNEL);
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

void exit_od_table(struct task_struct* t)
{
	int i;

	if (t->od_table) {
		for (i = 0; i < MAX_OBJECT_DESCRIPTORS; i++)
			if (t->od_table[i].used)
				put_od_entry(t->od_table + i);
		kfree(t->od_table);
		t->od_table = NULL;
	}
}

static int do_sys_od_open(struct file* file, obj_type_t type, int id,
			  void* __user config)
{
	int idx = 0, err;
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
		obj = alloc_inode_obj(inode, type, id);
	if (!obj) {
		idx = -ENOMEM;
		entry->used = 0;
	} else {
		entry->obj   = obj;
		entry->extra = NULL;
		idx = entry - current->od_table;
	}

	mutex_unlock(&inode->i_obj_mutex);

	err = fdso_open(entry, config);
	if (err < 0) {
		/* The class rejected the open call.
		 * We need to clean up and tell user space.
		 */
		put_od_entry(entry);
		idx = err;
	}

	return idx;
}


struct od_table_entry* __od_lookup(int od)
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


asmlinkage int sys_od_open(int fd, int type, int obj_id, void* __user config)
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


asmlinkage int sys_od_close(int od)
{
	int ret = -EINVAL;
	struct task_struct *t = current;

	if (od < 0 || od >= MAX_OBJECT_DESCRIPTORS)
		return ret;

	if (!t->od_table || !t->od_table[od].used)
		return ret;


	/* give the class a chance to reject the close
	 */
	ret = fdso_close(t->od_table + od);
	if (ret == 0)
		ret = put_od_entry(t->od_table + od);

	return ret;
}
