#ifndef _LITMUS_FTDEV_H_
#define	_LITMUS_FTDEV_H_

#include <litmus/feather_trace.h>
#include <litmus/feather_buffer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>

#define MAX_FTDEV_MINORS NR_CPUS

#define FTDEV_ENABLE_CMD 	0
#define FTDEV_DISABLE_CMD 	1

struct ftdev;

/* return 0 if buffer can be opened, otherwise -$REASON */
typedef int  (*ftdev_can_open_t)(struct ftdev* dev, unsigned int buf_no);
/* return 0 on success, otherwise -$REASON */
typedef int  (*ftdev_alloc_t)(struct ftdev* dev, unsigned int buf_no);
typedef void (*ftdev_free_t)(struct ftdev* dev, unsigned int buf_no);


struct ftdev_event;

struct ftdev_minor {
	struct ft_buffer*	buf;
	unsigned int		readers;
	struct mutex		lock;
	/* FIXME: filter for authorized events */
	struct ftdev_event*	events;
};

struct ftdev {
	struct cdev		cdev;
	/* FIXME: don't waste memory, allocate dynamically */
	struct ftdev_minor	minor[MAX_FTDEV_MINORS];
	unsigned int		minor_cnt;
	ftdev_alloc_t		alloc;
	ftdev_free_t		free;
	ftdev_can_open_t	can_open;
};

struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size);
void free_ft_buffer(struct ft_buffer* buf);

void ftdev_init(struct ftdev* ftdev, struct module* owner);
int register_ftdev(struct ftdev* ftdev, const char* name, int major);

#endif
