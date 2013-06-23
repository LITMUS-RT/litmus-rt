#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/vmalloc.h>

#include <litmus/feather_trace.h>
#include <litmus/ftdev.h>

struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size)
{
	struct ft_buffer* buf;
	size_t total = (size + 1) * count;
	char* mem;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return NULL;


	mem = vmalloc(total);

	if (!mem) {
		kfree(buf);
		return NULL;
	}

	if (!init_ft_buffer(buf, count, size,
			    mem + (count * size),  /* markers at the end */
			    mem)) {                /* buffer objects     */
		vfree(mem);
		kfree(buf);
		return NULL;
	}
	return buf;
}

void free_ft_buffer(struct ft_buffer* buf)
{
	if (buf) {
		vfree(buf->buffer_mem);
		kfree(buf);
	}
}

struct ftdev_event {
	int id;
	struct ftdev_event* next;
};

static int activate(struct ftdev_event** chain, int id)
{
	struct ftdev_event* ev = kmalloc(sizeof(*ev), GFP_KERNEL);
	if (ev) {
		printk(KERN_INFO
		       "Enabling feather-trace event %d.\n", (int) id);
		ft_enable_event(id);
		ev->id = id;
		ev->next = *chain;
		*chain    = ev;
	}
	return ev ? 0 : -ENOMEM;
}

static void deactivate(struct ftdev_event** chain, int id)
{
	struct ftdev_event **cur = chain;
	struct ftdev_event *nxt;
	while (*cur) {
		if ((*cur)->id == id) {
			nxt   = (*cur)->next;
			kfree(*cur);
			*cur  = nxt;
			printk(KERN_INFO
			       "Disabling feather-trace event %d.\n", (int) id);
			ft_disable_event(id);
			break;
		}
		cur = &(*cur)->next;
	}
}

static int ftdev_open(struct inode *in, struct file *filp)
{
	struct ftdev* ftdev;
	struct ftdev_minor* ftdm;
	unsigned int buf_idx = iminor(in);
	int err = 0;

	ftdev = container_of(in->i_cdev, struct ftdev, cdev);

	if (buf_idx >= ftdev->minor_cnt) {
		err = -ENODEV;
		goto out;
	}
	if (ftdev->can_open && (err = ftdev->can_open(ftdev, buf_idx)))
		goto out;

	ftdm = ftdev->minor + buf_idx;
	ftdm->ftdev = ftdev;
	filp->private_data = ftdm;

	if (mutex_lock_interruptible(&ftdm->lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	if (!ftdm->readers && ftdev->alloc)
		err = ftdev->alloc(ftdev, buf_idx);
	if (0 == err)
		ftdm->readers++;

	mutex_unlock(&ftdm->lock);
out:
	return err;
}

static int ftdev_release(struct inode *in, struct file *filp)
{
	struct ftdev* ftdev;
	struct ftdev_minor* ftdm;
	unsigned int buf_idx = iminor(in);
	int err = 0;

	ftdev = container_of(in->i_cdev, struct ftdev, cdev);

	if (buf_idx >= ftdev->minor_cnt) {
		err = -ENODEV;
		goto out;
	}
	ftdm = ftdev->minor + buf_idx;

	if (mutex_lock_interruptible(&ftdm->lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	if (ftdm->readers == 1) {
		while (ftdm->events)
			deactivate(&ftdm->events, ftdm->events->id);

		/* wait for any pending events to complete */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ);

		printk(KERN_ALERT "Failed trace writes: %u\n",
		       ftdm->buf->failed_writes);

		if (ftdev->free)
			ftdev->free(ftdev, buf_idx);
	}

	ftdm->readers--;
	mutex_unlock(&ftdm->lock);
out:
	return err;
}

/* based on ft_buffer_read
 * @returns < 0 : page fault
 *          = 0 : no data available
 *          = 1 : one slot copied
 */
static int ft_buffer_copy_to_user(struct ft_buffer* buf, char __user *dest)
{
	unsigned int idx;
	int err = 0;
	if (buf->free_count != buf->slot_count) {
		/* data available */
		idx = buf->read_idx % buf->slot_count;
		if (buf->slots[idx] == SLOT_READY) {
			err = copy_to_user(dest, ((char*) buf->buffer_mem) +
					   idx * buf->slot_size,
					   buf->slot_size);
			if (err == 0) {
				/* copy ok */
				buf->slots[idx] = SLOT_FREE;
				buf->read_idx++;
				fetch_and_inc(&buf->free_count);
				err = 1;
			}
		}
	}
	return err;
}

static ssize_t ftdev_read(struct file *filp,
			  char __user *to, size_t len, loff_t *f_pos)
{
	/* 	we ignore f_pos, this is strictly sequential */

	ssize_t err = 0;
	size_t chunk;
	int copied;
	struct ftdev_minor* ftdm = filp->private_data;

	if (mutex_lock_interruptible(&ftdm->lock)) {
		err = -ERESTARTSYS;
		goto out;
	}


	chunk = ftdm->buf->slot_size;
	while (len >= chunk) {
		copied = ft_buffer_copy_to_user(ftdm->buf, to);
		if (copied == 1) {
			len    -= chunk;
			to     += chunk;
			err    += chunk;
	        } else if (err == 0 && copied == 0 && ftdm->events) {
			/* Only wait if there are any events enabled and only
			 * if we haven't copied some data yet. We cannot wait
			 * here with copied data because that data would get
			 * lost if the task is interrupted (e.g., killed).
			 */
			mutex_unlock(&ftdm->lock);
			set_current_state(TASK_INTERRUPTIBLE);

			schedule_timeout(50);

			if (signal_pending(current)) {
				if (err == 0)
					/* nothing read yet, signal problem */
					err = -ERESTARTSYS;
				goto out;
			}
			if (mutex_lock_interruptible(&ftdm->lock)) {
				err = -ERESTARTSYS;
				goto out;
			}
		} else if (copied < 0) {
			/* page fault */
			err = copied;
			break;
		} else
			/* nothing left to get, return to user space */
			break;
	}
	mutex_unlock(&ftdm->lock);
out:
	return err;
}

static long ftdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long err = -ENOIOCTLCMD;
	struct ftdev_minor* ftdm = filp->private_data;

	if (mutex_lock_interruptible(&ftdm->lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	/* FIXME: check id against list of acceptable events */

	switch (cmd) {
	case  FTDEV_ENABLE_CMD:
		if (activate(&ftdm->events, arg))
			err = -ENOMEM;
		else
			err = 0;
		break;

	case FTDEV_DISABLE_CMD:
		deactivate(&ftdm->events, arg);
		err = 0;
		break;

	case FTDEV_CALIBRATE:
		if (ftdm->ftdev->calibrate) {
			err = ftdm->ftdev->calibrate(ftdm->ftdev, iminor(filp->f_dentry->d_inode), arg);
		}
		break;

	default:
		printk(KERN_DEBUG "ftdev: strange ioctl (%u, %lu)\n", cmd, arg);
	};

	mutex_unlock(&ftdm->lock);
out:
	return err;
}

static ssize_t ftdev_write(struct file *filp, const char __user *from,
			   size_t len, loff_t *f_pos)
{
	struct ftdev_minor* ftdm = filp->private_data;
	ssize_t err = -EINVAL;
	struct ftdev* ftdev = ftdm->ftdev;

	/* dispatch write to buffer-specific code, if available */
	if (ftdev->write)
		err = ftdev->write(ftdm->buf, len, from);

	return err;
}

struct file_operations ftdev_fops = {
	.owner   = THIS_MODULE,
	.open    = ftdev_open,
	.release = ftdev_release,
	.write   = ftdev_write,
	.read    = ftdev_read,
	.unlocked_ioctl = ftdev_ioctl,
};

int ftdev_init(	struct ftdev* ftdev, struct module* owner,
		const int minor_cnt, const char* name)
{
	int i, err;

	BUG_ON(minor_cnt < 1);

	cdev_init(&ftdev->cdev, &ftdev_fops);
	ftdev->name = name;
	ftdev->minor_cnt = minor_cnt;
	ftdev->cdev.owner = owner;
	ftdev->cdev.ops = &ftdev_fops;
	ftdev->alloc    = NULL;
	ftdev->free     = NULL;
	ftdev->can_open = NULL;
	ftdev->write	= NULL;
	ftdev->calibrate = NULL;

	ftdev->minor = kcalloc(ftdev->minor_cnt, sizeof(*ftdev->minor),
			GFP_KERNEL);
	if (!ftdev->minor) {
		printk(KERN_WARNING "ftdev(%s): Could not allocate memory\n",
			ftdev->name);
		err = -ENOMEM;
		goto err_out;
	}

	for (i = 0; i < ftdev->minor_cnt; i++) {
		mutex_init(&ftdev->minor[i].lock);
		ftdev->minor[i].readers = 0;
		ftdev->minor[i].buf     = NULL;
		ftdev->minor[i].events  = NULL;
	}

	ftdev->class = class_create(owner, ftdev->name);
	if (IS_ERR(ftdev->class)) {
		err = PTR_ERR(ftdev->class);
		printk(KERN_WARNING "ftdev(%s): "
			"Could not create device class.\n", ftdev->name);
		goto err_dealloc;
	}

	return 0;

err_dealloc:
	kfree(ftdev->minor);
err_out:
	return err;
}

/*
 * Destroy minor devices up to, but not including, up_to.
 */
static void ftdev_device_destroy(struct ftdev* ftdev, unsigned int up_to)
{
	dev_t minor_cntr;

	if (up_to < 1)
		up_to = (ftdev->minor_cnt < 1) ? 0 : ftdev->minor_cnt;

	for (minor_cntr = 0; minor_cntr < up_to; ++minor_cntr)
		device_destroy(ftdev->class, MKDEV(ftdev->major, minor_cntr));
}

void ftdev_exit(struct ftdev* ftdev)
{
	printk("ftdev(%s): Exiting\n", ftdev->name);
	ftdev_device_destroy(ftdev, -1);
	cdev_del(&ftdev->cdev);
	unregister_chrdev_region(MKDEV(ftdev->major, 0), ftdev->minor_cnt);
	class_destroy(ftdev->class);
	kfree(ftdev->minor);
}

int register_ftdev(struct ftdev* ftdev)
{
	struct device **device;
	dev_t trace_dev_tmp, minor_cntr;
	int err;

	err = alloc_chrdev_region(&trace_dev_tmp, 0, ftdev->minor_cnt,
			ftdev->name);
	if (err) {
		printk(KERN_WARNING "ftdev(%s): "
		       "Could not allocate char. device region (%d minors)\n",
		       ftdev->name, ftdev->minor_cnt);
		goto err_out;
	}

	ftdev->major = MAJOR(trace_dev_tmp);

	err = cdev_add(&ftdev->cdev, trace_dev_tmp, ftdev->minor_cnt);
	if (err) {
		printk(KERN_WARNING "ftdev(%s): "
		       "Could not add cdev for major %u with %u minor(s).\n",
		       ftdev->name, ftdev->major, ftdev->minor_cnt);
		goto err_unregister;
	}

	/* create the minor device(s) */
	for (minor_cntr = 0; minor_cntr < ftdev->minor_cnt; ++minor_cntr)
	{
		trace_dev_tmp = MKDEV(ftdev->major, minor_cntr);
		device = &ftdev->minor[minor_cntr].device;

		*device = device_create(ftdev->class, NULL, trace_dev_tmp, NULL,
				"litmus/%s%d", ftdev->name, minor_cntr);
		if (IS_ERR(*device)) {
			err = PTR_ERR(*device);
			printk(KERN_WARNING "ftdev(%s): "
				"Could not create device major/minor number "
				"%u/%u\n", ftdev->name, ftdev->major,
				minor_cntr);
			printk(KERN_WARNING "ftdev(%s): "
				"will attempt deletion of allocated devices.\n",
				ftdev->name);
			goto err_minors;
		}
	}

	return 0;

err_minors:
	ftdev_device_destroy(ftdev, minor_cntr);
	cdev_del(&ftdev->cdev);
err_unregister:
	unregister_chrdev_region(MKDEV(ftdev->major, 0), ftdev->minor_cnt);
err_out:
	return err;
}
