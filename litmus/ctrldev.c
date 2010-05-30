#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

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
	unsigned long pfn;

	struct page* ctrl = virt_to_page(tsk_rt(t)->ctrl_page);

	/* Increase ref count. Is decreased when vma is destroyed. */
	get_page(ctrl);

	/* compute page frame number */
	pfn = page_to_pfn(ctrl);

	TRACE_CUR(CTRL_NAME
		  ": mapping %p (pfn:%lx, %lx) to 0x%lx (prot:%lx)\n",
		  tsk_rt(t)->ctrl_page, pfn, page_to_pfn(ctrl), vma->vm_start,
		  vma->vm_page_prot);

	/* Map it into the vma. Make sure to use PAGE_SHARED, otherwise
	 * userspace actually gets a copy-on-write page. */
	err = remap_pfn_range(vma, vma->vm_start, pfn, PAGE_SIZE, PAGE_SHARED);

	if (err)
		TRACE_CUR(CTRL_NAME ": remap_pfn_range() failed (%d)\n", err);

	return err;
}

static void litmus_ctrl_vm_close(struct vm_area_struct* vma)
{
	TRACE_CUR("%s flags=0x%x prot=0x%x\n", __FUNCTION__,
		  vma->vm_flags, vma->vm_page_prot);

	TRACE_CUR(CTRL_NAME
		  ": %p:%p vma:%p vma->vm_private_data:%p closed.\n",
		  (void*) vma->vm_start, (void*) vma->vm_end, vma,
		  vma->vm_private_data, current->comm,
		  current->pid);
}

static int litmus_ctrl_vm_fault(struct vm_area_struct* vma,
				      struct vm_fault* vmf)
{
	/* This function should never be called, since
	 * all pages should have been mapped by mmap()
	 * already. */
	TRACE_CUR("%s flags=0x%x\n", __FUNCTION__, vma->vm_flags);

	/* nope, you only get one page */
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
	/* this mapping should not be kept across forks,
	 * and cannot be expanded */
	vma->vm_flags |= VM_DONTCOPY | VM_DONTEXPAND;

	err = alloc_ctrl_page(current);
	if (!err)
		err = map_ctrl_page(current, vma);

	TRACE_CUR("%s flags=0x%x prot=0x%lx\n",
		  __FUNCTION__, vma->vm_flags, vma->vm_page_prot);

	return err;
}

static struct file_operations litmus_ctrl_fops = {
	.owner = THIS_MODULE,
	.mmap  = litmus_ctrl_mmap,
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
