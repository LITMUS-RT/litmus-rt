#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/module.h>

#include <litmus/litmus.h>
#include <litmus/feather_trace.h>
#include <litmus/ftdev.h>

struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size)
{
	struct ft_buffer* buf;
	size_t total = (size + 1) * count;
	char* mem;
	int order = 0, pages = 1;

	buf = kmalloc(sizeof(struct ft_buffer), GFP_KERNEL);
	if (!buf)
		return NULL;

	total = (total / PAGE_SIZE) + (total % PAGE_SIZE != 0);
	while (pages < total) {
		order++;
		pages *= 2;
	}

	mem = (char*) __get_free_pages(GFP_KERNEL, order);
	if (!mem) {
		kfree(buf);
		return NULL;
	}

	if (!init_ft_buffer(buf, count, size,
			    mem + (count * size),  /* markers at the end */
			    mem)) {                /* buffer objects     */
		free_pages((unsigned long) mem, order);
		kfree(buf);
		return NULL;
	}
	return buf;
}

void free_ft_buffer(struct ft_buffer* buf)
{
	int order = 0, pages = 1;
	size_t total;

	if (buf) {
		total = (buf->slot_size + 1) * buf->slot_count;
		total = (total / PAGE_SIZE) + (total % PAGE_SIZE != 0);
		while (pages < total) {
			order++;
			pages *= 2;
		}
		free_pages((unsigned long) buf->buffer_mem, order);
		kfree(buf);
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
	ftdm = ftdev->minor + buf_idx;
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
		/*FIXME: disable events */
		ftdm->active_events = 0;

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
	        } else if (copied == 0 && ftdm->active_events) {
			/* only wait if there are any events enabled */
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(50);
			if (signal_pending(current)) {
				if (err == 0)
					/* nothing read yet, signal problem */
					err = -ERESTARTSYS;
				break;
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

typedef uint32_t cmd_t;

static ssize_t ftdev_write(struct file *filp, const char __user *from,
			   size_t len, loff_t *f_pos)
{
	struct ftdev_minor* ftdm = filp->private_data;
	ssize_t err = -EINVAL;
	cmd_t cmd;
	cmd_t id;

	if (len % sizeof(cmd_t) || len < 2 * sizeof(cmd_t))
		goto out;

	if (copy_from_user(&cmd, from, sizeof(cmd_t))) {
		err = -EFAULT;
	        goto out;
	}
	len  -= sizeof(cmd_t);
	from += sizeof(cmd_t);

	if (cmd != FTDEV_ENABLE_CMD && cmd != FTDEV_DISABLE_CMD)
		goto out;

	if (mutex_lock_interruptible(&ftdm->lock)) {
		err = -ERESTARTSYS;
		goto out;
	}

	err = sizeof(cmd_t);
	while (len) {
		if (copy_from_user(&id, from, sizeof(cmd_t))) {
			err = -EFAULT;
			goto out_unlock;
		}
		/* FIXME: check id against list of acceptable events */
		/* FIXME: track which events must be disable at release time */
		len  -= sizeof(cmd_t);
		from += sizeof(cmd_t);
		if (cmd == FTDEV_ENABLE_CMD) {
			printk(KERN_INFO
			       "Disabling feather-trace event %d.\n", (int) id);
			ft_disable_event(id);
			ftdm->active_events--;
		} else {
			printk(KERN_INFO
			       "Enabling feather-trace event %d.\n", (int) id);
			ft_enable_event(id);
			ftdm->active_events++;
		}
		err += sizeof(cmd_t);
	}

out_unlock:
	mutex_unlock(&ftdm->lock);
out:
	return err;
}

struct file_operations ftdev_fops = {
	.owner   = THIS_MODULE,
	.open    = ftdev_open,
	.release = ftdev_release,
	.write   = ftdev_write,
	.read    = ftdev_read,
};


void ftdev_init(struct ftdev* ftdev)
{
	int i;
	ftdev->cdev.owner = THIS_MODULE;
	ftdev->cdev.ops = &ftdev_fops;
	ftdev->minor_cnt  = 0;
	for (i = 0; i < MAX_FTDEV_MINORS; i++) {
		mutex_init(&ftdev->minor[i].lock);
		ftdev->minor[i].readers = 0;
		ftdev->minor[i].buf = NULL;
		ftdev->minor[i].active_events = 0;
	}
	ftdev->alloc = NULL;
	ftdev->free  = NULL;
}

int register_ftdev(struct ftdev* ftdev, const char* name, int major)
{
	dev_t   trace_dev;
	int error = 0;

	trace_dev = MKDEV(major, 0);
	error     = register_chrdev_region(trace_dev, ftdev->minor_cnt, name);
	if (error)
	{
		printk(KERN_WARNING "ftdev(%s): "
		       "Could not register major/minor number %d/%u\n",
		       name, major, ftdev->minor_cnt);
		return error;
	}
	error = cdev_add(&ftdev->cdev, trace_dev, ftdev->minor_cnt);
	if (error) {
		printk(KERN_WARNING "ftdev(%s): "
		       "Could not add cdev for major/minor = %d/%u.\n",
		       name, major, ftdev->minor_cnt);
		return error;
	}
	return error;
}
