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

typedef int  (*ftdev_alloc_t)(struct ftdev* dev, unsigned int buf_no);
typedef void (*ftdev_free_t)(struct ftdev* dev, unsigned int buf_no);

struct ftdev_minor {
	struct ft_buffer*	buf;
	unsigned int		readers;
	struct mutex		lock;
	unsigned		active_events;
};

struct ftdev {
	struct cdev		cdev;
	/* FIXME: don't waste memory, allocate dynamically */
	struct ftdev_minor	minor[MAX_FTDEV_MINORS];
	unsigned int		minor_cnt;
	/* FIXME: track enabled/disabled events */
	ftdev_alloc_t		alloc;
	ftdev_free_t		free;
};

struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size);
void free_ft_buffer(struct ft_buffer* buf);

void ftdev_init(struct ftdev* ftdev);
int register_ftdev(struct ftdev* ftdev, const char* name, int major);

#endif
