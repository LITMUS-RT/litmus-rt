#ifndef _LITMUS_FTDEV_H_
#define	_LITMUS_FTDEV_H_

#include <litmus/feather_trace.h>
#include <litmus/feather_buffer.h>
#include <linux/mutex.h>
#include <linux/cdev.h>

#define FTDEV_ENABLE_CMD 	0
#define FTDEV_DISABLE_CMD 	1
#define FTDEV_CALIBRATE		0x1410

struct ftdev;

/* return 0 if buffer can be opened, otherwise -$REASON */
typedef int  (*ftdev_can_open_t)(struct ftdev* dev, unsigned int buf_no);
/* return 0 on success, otherwise -$REASON */
typedef int  (*ftdev_alloc_t)(struct ftdev* dev, unsigned int buf_no);
typedef void (*ftdev_free_t)(struct ftdev* dev, unsigned int buf_no);
typedef long (*ftdev_calibrate_t)(struct ftdev* dev, unsigned int buf_no, unsigned long user_arg);
/* Let devices handle writes from userspace. No synchronization provided. */
typedef ssize_t (*ftdev_write_t)(struct ft_buffer* buf, size_t len, const char __user *from);

struct ftdev_event;

struct ftdev_minor {
	struct ft_buffer*	buf;
	unsigned int		readers;
	struct mutex		lock;
	/* FIXME: filter for authorized events */
	struct ftdev_event*	events;
	struct device*		device;
	struct ftdev*		ftdev;
};

struct ftdev {
	dev_t			major;
	struct cdev		cdev;
	struct class*		class;
	const char*		name;
	struct ftdev_minor*	minor;
	unsigned int		minor_cnt;
	ftdev_alloc_t		alloc;
	ftdev_free_t		free;
	ftdev_can_open_t	can_open;
	ftdev_write_t		write;
	ftdev_calibrate_t	calibrate;
};

struct ft_buffer* alloc_ft_buffer(unsigned int count, size_t size);
void free_ft_buffer(struct ft_buffer* buf);

int ftdev_init(	struct ftdev* ftdev, struct module* owner,
		const int minor_cnt, const char* name);
void ftdev_exit(struct ftdev* ftdev);
int register_ftdev(struct ftdev* ftdev);

#endif
