/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dmabuf: a buffer that one driver can hand to another through a file
 * descriptor, without a copy.
 *
 * An exporter (a V4L2 queue, a GPU driver) wraps its memory in a struct dmabuf
 * and gives userland a descriptor for it; an importer (another driver, or the
 * same one) turns the descriptor back into the struct dmabuf and asks it for
 * what it needs: a kernel address (software devices), the physical segments (a
 * device that DMAs), or a user mapping. The descriptor can be passed to
 * another process like any other.
 *
 * Reference counting: dmabuf_alloc() returns one reference, held by the caller.
 * The file made by dmabuf_to_fd() holds another. dmabuf_from_fd() returns a
 * reference for the importer. ops->release runs when the last one is dropped.
 *
 * Not here yet, on purpose: per-device attachments and DMA mappings (needed
 * for IOMMUs and for cache maintenance between two devices on a non-coherent
 * system), implicit synchronisation with fences, and DMA_BUF_IOCTL_SYNC. They
 * come with the first device that needs them.
 */
#ifndef _DEV_DMABUF_DMABUF_H_
#define	_DEV_DMABUF_DMABUF_H_

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/types.h>
#include <vm/vm.h>

struct dmabuf;
struct sglist;
struct thread;

struct dmabuf_ops {
	/*
	 * A user mapping: return a referenced vm object for [*offset, *offset +
	 * size) of the buffer, as d_mmap_single does; *offset is the offset into
	 * that object. NULL: the buffer cannot be mapped.
	 */
	int	(*mmap)(struct dmabuf *, vm_ooffset_t *offset, vm_size_t size,
		    vm_object_t *objp, int nprot);
	/* The kernel address of the whole buffer, valid while a reference is held. */
	void	*(*vmap)(struct dmabuf *);
	/* The physical segments of the buffer, as a new sglist (the caller frees it). */
	int	(*sglist)(struct dmabuf *, struct sglist **);
	/* The last reference is gone: free the memory and the structure. */
	void	(*release)(struct dmabuf *);
};

struct dmabuf {
	const struct dmabuf_ops	*ops;
	void			*priv;		/* the exporter's */
	size_t			size;
	u_int			refs;
};

/* A new buffer, one reference held by the caller, no descriptor yet. */
struct dmabuf *dmabuf_alloc(const struct dmabuf_ops *ops, size_t size, void *priv);
void dmabuf_hold(struct dmabuf *);
void dmabuf_put(struct dmabuf *);

/*
 * A descriptor for the buffer in td's table. oflags: O_CLOEXEC and the access
 * mode (O_RDWR if none). The file takes its own reference; the caller keeps
 * and still has to drop its own.
 */
int dmabuf_to_fd(struct dmabuf *, int oflags, struct thread *td, int *fdp);
/* The buffer behind a descriptor, referenced for the caller (EBADF/EINVAL if it is not one). */
int dmabuf_from_fd(int fd, struct thread *td, struct dmabuf **bufp);

/* NULL if the exporter has no kernel mapping. */
void *dmabuf_vmap(struct dmabuf *);
int dmabuf_sglist(struct dmabuf *, struct sglist **);

#endif /* _KERNEL */

#endif /* _DEV_DMABUF_DMABUF_H_ */
