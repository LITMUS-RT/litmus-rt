#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>


#include <litmus/litmus.h>

/* only one page for now, but we might want to add a RO version at some point */

#define CTRL_NAME        "litmus/ctrl"

/* allocate t->rt_param.ctrl_page*/
static int alloc_ctrl_page(struct task_struct *t)
{
	int err = 0;

	/* only allocate if the task doesn't have one yet */
	if (!tsk_rt(t)->ctrl_page) {
		tsk_rt(t)->ctrl_page = (void*) get_zeroed_page(GFP_KERNEL);
		if (!tsk_rt(t)->ctrl_page)
			err = -ENOMEM;
		/* will get de-allocated in task teardown */
		TRACE_TASK(t, "%s ctrl_page = %p\n", __FUNCTION__,
			   tsk_rt(t)->ctrl_page);
	}
	return err;
}

static int map_ctrl_page(struct task_struct *t, struct vm_area_struct* vma)
{
	int err;

	struct page* ctrl = virt_to_page(tsk_rt(t)->ctrl_page);

	TRACE_CUR(CTRL_NAME
		  ": mapping %p (pfn:%lx) to 0x%lx (prot:%lx)\n",
		  tsk_rt(t)->ctrl_page,page_to_pfn(ctrl), vma->vm_start,
		  vma->vm_page_prot);

	/* Map it into the vma. */
	err = vm_insert_page(vma, vma->vm_start, ctrl);

	if (err)
		TRACE_CUR(CTRL_NAME ": vm_insert_page() failed (%d)\n", err);

	return err;
}

static void litmus_ctrl_vm_close(struct vm_area_struct* vma)
{
	TRACE_CUR("%s flags=0x%x prot=0x%x\n", __FUNCTION__,
		  vma->vm_flags, vma->vm_page_prot);

	TRACE_CUR(CTRL_NAME
		  ": %p:%p vma:%p vma->vm_private_data:%p closed.\n",
		  (void*) vma->vm_start, (void*) vma->vm_end, vma,
		  vma->vm_private_data);
}

static int litmus_ctrl_vm_fault(struct vm_area_struct* vma,
				      struct vm_fault* vmf)
{
	TRACE_CUR("%s flags=0x%x (off:%ld)\n", __FUNCTION__,
		  vma->vm_flags, vmf->pgoff);

	/* This function should never be called, since all pages should have
	 * been mapped by mmap() already. */
	WARN_ONCE(1, "Page faults should be impossible in the control page\n");

	return VM_FAULT_SIGBUS;
}

static struct vm_operations_struct litmus_ctrl_vm_ops = {
	.close = litmus_ctrl_vm_close,
	.fault = litmus_ctrl_vm_fault,
};

static int litmus_ctrl_mmap(struct file* filp, struct vm_area_struct* vma)
{
	int err = 0;

	/* first make sure mapper knows what he's doing */

	/* you can only get one page */
	if (vma->vm_end - vma->vm_start != PAGE_SIZE)
		return -EINVAL;

	/* you can only map the "first" page */
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	/* you can't share it with anyone */
	if (vma->vm_flags & (VM_MAYSHARE | VM_SHARED))
		return -EINVAL;

	vma->vm_ops = &litmus_ctrl_vm_ops;
	/* This mapping should not be kept across forks,
	 * cannot be expanded, and is not a "normal" page. */
	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND | VM_READ | VM_WRITE;

	/* We don't want the first write access to trigger a "minor" page fault
	 * to mark the page as dirty.  This is transient, private memory, we
	 * don't care if it was touched or not. PAGE_SHARED means RW access, but
	 * not execute, and avoids copy-on-write behavior.
	 * See protection_map in mmap.c.  */
	vma->vm_page_prot = PAGE_SHARED;

	err = alloc_ctrl_page(current);
	if (!err)
		err = map_ctrl_page(current, vma);

	TRACE_CUR("%s flags=0x%x prot=0x%lx\n",
		  __FUNCTION__, vma->vm_flags, vma->vm_page_prot);

	return err;
}

/* LITMUS^RT system calls */

asmlinkage long sys_set_rt_task_param(pid_t pid, struct rt_task __user * param);
asmlinkage long sys_get_rt_task_param(pid_t pid, struct rt_task __user * param);
asmlinkage long sys_reservation_create(int type, void __user *config);
asmlinkage long sys_get_current_budget(lt_t __user * _expended, lt_t __user *_remaining);
asmlinkage long sys_null_call(cycles_t __user *ts);
asmlinkage long sys_od_open(int fd, int type, int obj_id, void* __user config);
asmlinkage long sys_od_close(int od);
asmlinkage long sys_complete_job(void);
asmlinkage long sys_litmus_lock(int lock_od);
asmlinkage long sys_litmus_unlock(int lock_od);
asmlinkage long sys_wait_for_job_release(unsigned int job);
asmlinkage long sys_wait_for_ts_release(void);
asmlinkage long sys_release_ts(lt_t __user *__delay);

static long litmus_ctrl_ioctl(struct file *filp,
	unsigned int cmd, unsigned long arg)
{
	long err = -ENOIOCTLCMD;

	/* LITMUS^RT syscall emulation: we expose LITMUS^RT-specific operations
	 * via ioctl() to avoid merge conflicts with the syscall tables when
	 * rebasing LITMUS^RT. Whi this is not the most elegant way to expose
	 * syscall-like functionality, it helps with reducing the effort
	 * required to maintain LITMUS^RT out of tree.
	 */

	union litmus_syscall_args syscall_args;

	switch (cmd) {
	case LRT_set_rt_task_param:
	case LRT_get_rt_task_param:
	case LRT_reservation_create:
	case LRT_get_current_budget:
	case LRT_od_open:
		/* multiple arguments => need to get args via pointer */
		/* get syscall parameters */
		if (copy_from_user(&syscall_args, (void*) arg,
		                   sizeof(syscall_args))) {
			return -EFAULT;
		}

		switch (cmd) {
		case LRT_set_rt_task_param:
			return sys_set_rt_task_param(
				syscall_args.get_set_task_param.pid,
				syscall_args.get_set_task_param.param);
		case LRT_get_rt_task_param:
			return sys_get_rt_task_param(
				syscall_args.get_set_task_param.pid,
				syscall_args.get_set_task_param.param);
		case LRT_reservation_create:
			return sys_reservation_create(
				syscall_args.reservation_create.type,
				syscall_args.reservation_create.config);
		case LRT_get_current_budget:
			return sys_get_current_budget(
				syscall_args.get_current_budget.expended,
				syscall_args.get_current_budget.remaining);
		case LRT_od_open:
			return sys_od_open(
				syscall_args.od_open.fd,
				syscall_args.od_open.obj_type,
				syscall_args.od_open.obj_id,
				syscall_args.od_open.config);
		}


	case LRT_null_call:
		return sys_null_call((cycles_t __user *) arg);

	case LRT_od_close:
		return sys_od_close(arg);

	case LRT_complete_job:
		return sys_complete_job();

	case LRT_litmus_lock:
		return sys_litmus_lock(arg);

	case LRT_litmus_unlock:
		return sys_litmus_unlock(arg);

	case LRT_wait_for_job_release:
		return sys_wait_for_job_release(arg);

	case LRT_wait_for_ts_release:
		return sys_wait_for_ts_release();

	case LRT_release_ts:
		return sys_release_ts((lt_t __user *) arg);

	default:
		printk(KERN_DEBUG "ctrldev: strange ioctl (%u, %lu)\n", cmd, arg);
	};

	return err;
}

static struct file_operations litmus_ctrl_fops = {
	.owner = THIS_MODULE,
	.mmap  = litmus_ctrl_mmap,
	.unlocked_ioctl = litmus_ctrl_ioctl,
};

static struct miscdevice litmus_ctrl_dev = {
	.name  = CTRL_NAME,
	.minor = MISC_DYNAMIC_MINOR,
	.fops  = &litmus_ctrl_fops,
};

static int __init init_litmus_ctrl_dev(void)
{
	int err;

	BUILD_BUG_ON(sizeof(struct control_page) > PAGE_SIZE);

	BUILD_BUG_ON(sizeof(union np_flag) != sizeof(uint32_t));

	BUILD_BUG_ON(offsetof(struct control_page, sched.raw)
		     != LITMUS_CP_OFFSET_SCHED);
	BUILD_BUG_ON(offsetof(struct control_page, irq_count)
		     != LITMUS_CP_OFFSET_IRQ_COUNT);
	BUILD_BUG_ON(offsetof(struct control_page, ts_syscall_start)
		     != LITMUS_CP_OFFSET_TS_SC_START);
	BUILD_BUG_ON(offsetof(struct control_page, irq_syscall_start)
		     != LITMUS_CP_OFFSET_IRQ_SC_START);

	printk("Initializing LITMUS^RT control device.\n");
	err = misc_register(&litmus_ctrl_dev);
	if (err)
		printk("Could not allocate %s device (%d).\n", CTRL_NAME, err);
	return err;
}

static void __exit exit_litmus_ctrl_dev(void)
{
	misc_deregister(&litmus_ctrl_dev);
}

module_init(init_litmus_ctrl_dev);
module_exit(exit_litmus_ctrl_dev);
