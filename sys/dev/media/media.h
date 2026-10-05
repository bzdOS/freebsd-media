/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Native V4L2 for FreeBSD: kernel API for drivers.
 *
 * Layers (docs/specs/SPEC_media_v4l2_native.md in bzdOS/bsdOS):
 *
 *	vdev	a /dev/videoN node: ioctl dispatch, per-open state
 *	vbq	the buffer queue behind it: states, memory, streaming, wakeups
 *	vctrl	the controls framework
 *
 * A driver implements the kobj interfaces vdev_if.m and vbq_if.m, embeds a
 * struct vdev and a struct vbq (both start with KOBJ_FIELDS) in its softc and
 * registers the device. The core validates arguments before it calls a driver,
 * so a method only ever sees an index in range, a buffer in the right state,
 * and so on.
 */
#ifndef _DEV_MEDIA_MEDIA_H_
#define	_DEV_MEDIA_MEDIA_H_

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kobj.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/selinfo.h>
#include <sys/sx.h>
#include <sys/event.h>
#include <sys/time.h>
#include <machine/bus.h>
#include <vm/vm.h>

#include <dev/media/uapi/linux/videodev2.h>

struct vdev;
struct vbq;
struct vbuf;
struct vfh;
struct vctrl;
struct vctrl_handler;

/* ------------------------------------------------------------------ vctrl */

/*
 * One control. Standard controls get their name, type and flags from a table
 * in vctrl.c; the driver supplies range and default.
 */
struct vctrl {
	TAILQ_ENTRY(vctrl)	link;
	struct vctrl_handler	*hdl;
	uint32_t		id;
	uint32_t		type;		/* V4L2_CTRL_TYPE_* */
	uint32_t		flags;		/* V4L2_CTRL_FLAG_* */
	const char		*name;
	int64_t			min, max, def;
	uint64_t		step;
	const char * const	*menu;		/* menu names, index min..max */
	int64_t			val;		/* current value */
};

struct vctrl_handler {
	struct sx		lock;
	TAILQ_HEAD(, vctrl)	ctrls;		/* sorted by id */
	struct vdev		*vdev;
	int			error;		/* first creation error, sticky */
};

void vctrl_handler_init(struct vctrl_handler *);
void vctrl_handler_free(struct vctrl_handler *);
/* Standard integer/boolean/button control. Name, type and flags come from the id. */
struct vctrl *vctrl_new_std(struct vctrl_handler *, uint32_t id, int64_t min,
    int64_t max, uint64_t step, int64_t def);
/* Standard menu control; menu[min..max] are the item names. */
struct vctrl *vctrl_new_std_menu(struct vctrl_handler *, uint32_t id,
    int64_t max, int64_t def, const char * const *menu);
struct vctrl *vctrl_find(struct vctrl_handler *, uint32_t id);
/* The handler's current value, for a driver that applies several at once. */
int64_t vctrl_get(struct vctrl *);

/* ------------------------------------------------------------------- vbuf */

enum vbuf_state {
	VBUF_DEQUEUED,		/* owned by userland */
	VBUF_PREPARED,		/* PREPARE_BUF done, owned by userland */
	VBUF_QUEUED,		/* queued by userland, not yet given to the driver */
	VBUF_ACTIVE,		/* owned by the driver */
	VBUF_DONE,		/* completed, waiting for DQBUF */
	VBUF_ERROR,		/* completed with an error, waiting for DQBUF */
};

/*
 * One buffer. Lifetime is reference counted: the queue holds one reference and
 * every live mmap() object holds one, so memory that userland still has mapped
 * outlives close(2) and REQBUFS.
 */
struct vbuf {
	struct vbq		*q;		/* NULL once the queue let go of it */
	u_int			refs;
	u_int			nmaps;		/* live mmap objects (0 or 1) */
	uint32_t		index;
	enum vbuf_state		state;
	TAILQ_ENTRY(vbuf)	qentry;		/* q->queued while QUEUED/ACTIVE */
	TAILQ_ENTRY(vbuf)	dentry;		/* q->done while DONE/ERROR */

	/* what the driver fills in */
	uint32_t		bytesused;
	uint32_t		flags;		/* V4L2_BUF_FLAG_* the driver may set */
	uint32_t		sequence;
	struct timeval		timestamp;

	/* memory */
	size_t			size;
	void			*vaddr;
	bus_addr_t		paddr;
	bus_dma_tag_t		dmat;
	bus_dmamap_t		dmamap;
	vm_memattr_t		memattr;
	off_t			offset;		/* mmap offset handed to userland */

	void			*priv;		/* driver */
};

/* ---------------------------------------------------------------- vbq_if.m */
#include "vbq_if.h"

#define	VBQ_MAX_BUFFERS		32

struct vbq {
	KOBJ_FIELDS;
	struct vdev		*vdev;
	enum v4l2_buf_type	type;
	struct mtx		mtx;		/* queue state, done list, wakeups */
	struct selinfo		rsel;
	struct vbuf		*bufs[VBQ_MAX_BUFFERS];
	uint32_t		nbufs;
	TAILQ_HEAD(, vbuf)	queued;		/* from QBUF to DQBUF */
	TAILQ_HEAD(, vbuf)	done;		/* completed, in completion order */
	uint32_t		nqueued;	/* length of `queued` */
	uint32_t		nactive;	/* with the driver */
	uint32_t		sequence;
	uint32_t		min_queued;	/* buffers needed before start_streaming */
	bool			streaming;	/* STREAMON seen */
	bool			started;	/* start_streaming() was called */
	bool			error;		/* the driver reported a fatal error */
	off_t			next_offset;
	struct vfh		*owner;		/* fh that allocated the buffers */
};

/* Initialise a queue: `cls` is the driver's vbq_if class. */
void vbq_init(struct vbq *, struct vdev *, enum v4l2_buf_type, kobj_class_t cls);
void vbq_destroy(struct vbq *);
/*
 * A driver hands a buffer back from its interrupt handler or a callout.
 * `error` marks it V4L2_BUF_FLAG_ERROR. bytesused/timestamp/sequence are filled
 * in by the driver before the call; vbq_buf_done() fills in timestamp and
 * sequence if the driver left them zero.
 */
void vbq_buf_done(struct vbuf *, bool error);
bool vbq_is_streaming(struct vbq *);

/* --------------------------------------------------------------- vdev_if.m */
#include "vdev_if.h"

struct vdev {
	KOBJ_FIELDS;
	device_t		dev;		/* parent device, for the DMA tag */
	struct cdev		*cdev;
	int			unit;		/* /dev/video<unit> */
	uint32_t		device_caps;	/* V4L2_CAP_* of this node */
	struct sx		lock;		/* serialises ioctls */
	struct vbq		*queue;
	struct vctrl_handler	*ctrl_handler;
	TAILQ_HEAD(, vfh)	fhs;		/* protected by the core's global lock */
	u_int			nclosing;	/* file closes in progress */
	bool			registered;
	bool			dying;
};

/* Per-open state. */
struct vfh {
	TAILQ_ENTRY(vfh)	link;
	struct vdev		*vdev;		/* NULL after the device went away */
	uint32_t		input;
};

/*
 * Make the node /dev/video<N>. `v` must be zeroed and have its kobj class set
 * with kobj_init(); v->queue and v->ctrl_handler may be NULL.
 */
int vdev_register(struct vdev *v, device_t dev, uint32_t device_caps);
/* Remove the node, wait for ioctls in flight, and detach open files. */
void vdev_unregister(struct vdev *v);

#endif /* _KERNEL */

#endif /* _DEV_MEDIA_MEDIA_H_ */
