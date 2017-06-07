#include <linux/types.h>
#include <linux/module.h>
#include <asm/cacheflush.h>
#include <asm/sections.h>

#include <litmus/feather_trace.h>

/* the feather trace management functions assume
 * exclusive access to the event table
 */

#ifndef CONFIG_RELOCATABLE

#define BYTE_JUMP      0xeb
#define BYTE_JUMP_LEN  0x02

/* for each event, there is an entry in the event table */
struct trace_event {
	long 	id;
	long	count;
	long	start_addr;
	long	end_addr;
};

extern struct trace_event  __start___event_table[];
extern struct trace_event  __stop___event_table[];


/* NOTE: The following two functions have been stolen from ftrace.c */

static inline int
within(unsigned long addr, unsigned long start, unsigned long end)
{
	return addr >= start && addr < end;
}

static unsigned long text_ip_addr(unsigned long ip)
{
	/*
	 * On x86_64, kernel text mappings are mapped read-only, so we use
	 * the kernel identity mapping instead of the kernel text mapping
	 * to modify the kernel text.
	 *
	 * For 32bit kernels, these mappings are same and we can use
	 * kernel identity mapping to modify code.
	 */
	if (within(ip, (unsigned long)_text, (unsigned long)_etext))
		ip = (unsigned long)__va(__pa_symbol(ip));

	return ip;
}

/* Workaround: if no events are defined, then the event_table section does not
 * exist and the above references cause linker errors. This could probably be
 * fixed by adjusting the linker script, but it is easier to maintain for us if
 * we simply create a dummy symbol in the event table section.
 */
int __event_table_dummy[0] __attribute__ ((section("__event_table")));

int ft_enable_event(unsigned long id)
{
	struct trace_event* te = __start___event_table;
	int count = 0;
	char* delta;
	unsigned char* instr;

	set_kernel_text_rw();
	set_all_modules_text_rw();

	while (te < __stop___event_table) {
		if (te->id == id && ++te->count == 1) {
			instr  = (unsigned char*) te->start_addr;
			/* make sure we don't clobber something wrong */
			if (*instr == BYTE_JUMP) {
				delta  = (unsigned char*) text_ip_addr(
						((unsigned long) te->start_addr)
						+ 1);
				*delta = 0;
			}
		}
		if (te->id == id)
			count++;
		te++;
	}

	set_all_modules_text_ro();
	set_kernel_text_ro();

	printk(KERN_DEBUG "ft_enable_event: enabled %d events\n", count);
	return count;
}

int ft_disable_event(unsigned long id)
{
	struct trace_event* te = __start___event_table;
	int count = 0;
	char* delta;
	unsigned char* instr;

	set_kernel_text_rw();
	set_all_modules_text_rw();

	while (te < __stop___event_table) {
		if (te->id == id && --te->count == 0) {
			instr  = (unsigned char*) te->start_addr;
			if (*instr == BYTE_JUMP) {
				delta  = (unsigned char*) text_ip_addr(
						((unsigned long) te->start_addr)
						+ 1);
				*delta = te->end_addr - te->start_addr -
					BYTE_JUMP_LEN;
			}
		}
		if (te->id == id)
			count++;
		te++;
	}

	set_all_modules_text_ro();
	set_kernel_text_ro();

	printk(KERN_DEBUG "ft_disable_event: disabled %d events\n", count);
	return count;
}

int ft_disable_all_events(void)
{
	struct trace_event* te = __start___event_table;
	int count = 0;
	char* delta;
	unsigned char* instr;

	set_kernel_text_rw();
	set_all_modules_text_rw();

	while (te < __stop___event_table) {
		if (te->count) {
			instr  = (unsigned char*) te->start_addr;
			if (*instr == BYTE_JUMP) {
				delta  = (unsigned char*) text_ip_addr(
						((unsigned long) te->start_addr)
						+ 1);
				*delta = te->end_addr - te->start_addr -
					BYTE_JUMP_LEN;
				te->count = 0;
				count++;
			}
		}
		te++;
	}

	set_all_modules_text_ro();
	set_kernel_text_ro();

	return count;
}

int ft_is_event_enabled(unsigned long id)
{
	struct trace_event* te = __start___event_table;

	while (te < __stop___event_table) {
		if (te->id == id)
			return te->count;
		te++;
	}
	return 0;
}

#endif
