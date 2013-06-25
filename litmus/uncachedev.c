#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <asm/page.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include <litmus/litmus.h>

/* device for allocating pages not cached by the CPU */

#define UNCACHE_NAME        "litmus/uncache"

void litmus_uncache_vm_open(struct vm_area_struct *vma)
{
}

void litmus_uncache_vm_close(struct vm_area_struct *vma)
{
}

int litmus_uncache_vm_fault(struct vm_area_struct* vma,
							struct vm_fault* vmf)
{
	/* modeled after SG DMA video4linux, but without DMA. */
	/* (see drivers/media/video/videobuf-dma-sg.c) */
	struct page *page;

	page = alloc_page(GFP_USER);
	if (!page)
		return VM_FAULT_OOM;

	clear_user_highpage(page, (unsigned long)vmf->virtual_address);
	vmf->page = page;

	return 0;
}

static struct vm_operations_struct litmus_uncache_vm_ops = {
	.open = litmus_uncache_vm_open,
	.close = litmus_uncache_vm_close,
	.fault = litmus_uncache_vm_fault,
};

static int litmus_uncache_mmap(struct file* filp, struct vm_area_struct* vma)
{
	/* first make sure mapper knows what he's doing */

	/* you can only map the "first" page */
	if (vma->vm_pgoff != 0)
		return -EINVAL;

	/* you can't share it with anyone */
	if (vma->vm_flags & (VM_MAYSHARE | VM_SHARED))
		return -EINVAL;

	/* cannot be expanded, and is not a "normal" page. */
	vma->vm_flags |= VM_DONTEXPAND;

	/* noncached pages are not explicitly locked in memory (for now). */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	vma->vm_ops = &litmus_uncache_vm_ops;

	return 0;
}

static struct file_operations litmus_uncache_fops = {
	.owner = THIS_MODULE,
	.mmap  = litmus_uncache_mmap,
};

static struct miscdevice litmus_uncache_dev = {
	.name  = UNCACHE_NAME,
	.minor = MISC_DYNAMIC_MINOR,
	.fops  = &litmus_uncache_fops,
	/* pages are not locked, so there is no reason why
	   anyone cannot allocate an uncache pages */
	.mode  = (S_IRUGO | S_IWUGO),
};

static int __init init_litmus_uncache_dev(void)
{
	int err;

	printk("Initializing LITMUS^RT uncache device.\n");
	err = misc_register(&litmus_uncache_dev);
	if (err)
		printk("Could not allocate %s device (%d).\n", UNCACHE_NAME, err);
	return err;
}

static void __exit exit_litmus_uncache_dev(void)
{
	misc_deregister(&litmus_uncache_dev);
}

module_init(init_litmus_uncache_dev);
module_exit(exit_litmus_uncache_dev);
