#ifndef _FEATHER_BUFFER_H_
#define _FEATHER_BUFFER_H_

/* requires UINT_MAX and memcpy */

#define SLOT_FREE	0
#define	SLOT_BUSY 	1
#define	SLOT_READY	2

struct ft_buffer {
	unsigned int	slot_count;
	unsigned int	slot_size;

	int 		free_count;
	unsigned int 	write_idx;
	unsigned int 	read_idx;

	char*		slots;
	void*		buffer_mem;
	unsigned int	failed_writes;
};

static inline int init_ft_buffer(struct ft_buffer*	buf,
				 unsigned int 		slot_count,
				 unsigned int 		slot_size,
				 char*			slots,
				 void* 			buffer_mem)
{
	int i = 0;
	if (!slot_count || UINT_MAX % slot_count != slot_count - 1) {
		/* The slot count must divide UNIT_MAX + 1 so that when it
		 * wraps around the index correctly points to 0.
		 */
		return 0;
	} else {
		buf->slot_count    = slot_count;
		buf->slot_size     = slot_size;
		buf->slots         = slots;
		buf->buffer_mem    = buffer_mem;
		buf->free_count    = slot_count;
		buf->write_idx     = 0;
		buf->read_idx      = 0;
		buf->failed_writes = 0;
		for (i = 0; i < slot_count; i++)
			buf->slots[i] = SLOT_FREE;
		return 1;
	}
}

static inline int ft_buffer_start_write(struct ft_buffer* buf, void **ptr)
{
	int free = fetch_and_dec(&buf->free_count);
	unsigned int idx;
	if (free <= 0) {
		fetch_and_inc(&buf->free_count);
		*ptr = 0;
		fetch_and_inc(&buf->failed_writes);
		return 0;
	} else {
		idx  = fetch_and_inc((int*) &buf->write_idx) % buf->slot_count;
		buf->slots[idx] = SLOT_BUSY;
		*ptr = ((char*) buf->buffer_mem) + idx * buf->slot_size;
		return 1;
	}
}

/* For single writer scenarios, with fewer atomic ops. */
static inline int ft_buffer_start_single_write(struct ft_buffer* buf, void **ptr)
{
	unsigned int idx;

	if (buf->free_count <= 0) {
		*ptr = 0;
		/* single writer: no atomicity needed */
		buf->failed_writes++;
		return 0;
	} else {
		/* free_count is positive, and can only increase since we are
		 * (by assumption) the only writer accessing the buffer.
		 */

		idx  = buf->write_idx++ % buf->slot_count;
		buf->slots[idx] = SLOT_BUSY;
		*ptr = ((char*) buf->buffer_mem) + idx * buf->slot_size;

		ft_atomic_dec(&buf->free_count);
		return 1;
	}
}

static inline void ft_buffer_finish_write(struct ft_buffer* buf, void *ptr)
{
	unsigned int idx = ((char*) ptr - (char*) buf->buffer_mem) / buf->slot_size;
	buf->slots[idx]  = SLOT_READY;
}


/* exclusive reader access is assumed */
static inline int ft_buffer_read(struct ft_buffer* buf, void* dest)
{
	unsigned int idx;
	if (buf->free_count == buf->slot_count)
		/* nothing available */
		return 0;
	idx = buf->read_idx % buf->slot_count;
	if (buf->slots[idx] == SLOT_READY) {
		memcpy(dest, ((char*) buf->buffer_mem) + idx * buf->slot_size,
		       buf->slot_size);
		buf->slots[idx] = SLOT_FREE;
		buf->read_idx++;
		fetch_and_inc(&buf->free_count);
		return 1;
	} else
		return 0;
}


#endif
