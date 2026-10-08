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
#include <dev/media/uapi/linux/media.h>
#include <dev/dmabuf/dmabuf.h>

/*
 * V4L2 says EBADR when a queue does not take requests, or only takes them.
 * FreeBSD has no EBADR; applications built here map it to the nearest name.
 */
#ifndef EBADR
#define	EBADR	EBADRPC
#endif

struct vdev;
struct vbq;
struct vbuf;
struct vfh;
struct vctrl;
struct vctrl_handler;
struct vmedia;
struct vreq;
struct vmedia_entity;
struct vmedia_intf;

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
	int64_t			val;		/* current value (scalar controls) */

	/*
	 * Array and compound controls carry their value as a payload of
	 * `max_elems` elements of `elem_size` bytes, `elems` of them in use (fewer
	 * than max_elems only for a dynamic array). Scalars have no payload.
	 */
	uint32_t		elem_size;
	uint32_t		elems, max_elems;
	uint32_t		nr_dims;
	uint32_t		dims[V4L2_CTRL_MAX_DIMS];
	uint8_t			*p_cur;		/* current payload, NULL for a scalar */
	uint8_t			*p_def;		/* default payload */
};

/* A driver-defined control (or a standard one with a payload). */
struct vctrl_config {
	uint32_t		id;
	const char		*name;
	uint32_t		type;		/* V4L2_CTRL_TYPE_* */
	uint32_t		flags;
	int64_t			min, max, def;	/* of an element, for integer-like types */
	uint64_t		step;
	uint32_t		elem_size;	/* 0: the type's own (U8 1, U16 2, U32/int 4) */
	uint32_t		nr_dims;	/* 0: a single element */
	uint32_t		dims[V4L2_CTRL_MAX_DIMS];	/* of which the product is max_elems */
	const char * const	*menu;
};

struct vctrl_handler {
	struct sx		lock;
	TAILQ_HEAD(, vctrl)	ctrls;		/* sorted by id */
	struct vdev		*vdev;
	struct vfh		*owner;		/* NULL: shared by every open file */
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
/* The same for a menu whose valid items start at `min` (items below it are not offered). */
struct vctrl *vctrl_new_std_menu_range(struct vctrl_handler *, uint32_t id, int64_t min,
    int64_t max, int64_t def, const char * const *menu);
/* A control from `cfg`: an array of integers or a compound (structure) control. */
struct vctrl *vctrl_new_custom(struct vctrl_handler *, const struct vctrl_config *cfg);
struct vctrl *vctrl_find(struct vctrl_handler *, uint32_t id);
/* Copy a payload control's current value out; `size` must be its full size. */
int vctrl_get_payload(struct vctrl *, void *out, size_t size);
/*
 * A driver about to run the job of buffer `b` calls this first: the controls
 * set in the request become the handler's current values (through the driver's
 * s_ctrl), and the request remembers the state of all controls, which is what
 * userland reads back once the request is complete. May sleep. A no-op for a
 * buffer that is not in a request.
 */
void vctrl_request_setup(struct vbuf *b, struct vctrl_handler *);
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
	VBUF_IN_REQUEST,	/* bound to a request that has not been queued yet */
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
	TAILQ_ENTRY(vbuf)	drvq;		/* the driver's own list (mem2mem: ready) */

	/* what the driver fills in */
	uint32_t		bytesused;
	uint32_t		flags;		/* V4L2_BUF_FLAG_* the driver may set */
	uint32_t		sequence;
	struct timeval		timestamp;

	/* an imported dmabuf: vaddr is the exporter's, we only hold a reference */
	struct dmabuf		*dbuf;
	int			dfd;		/* the descriptor userland queued, reported back */
	bool			imported;

	/* memory: one DMA segment (paddr, dmat, dmamap) or separate pages (vaddr only) */
	size_t			size;
	void			*vaddr;
	bus_addr_t		paddr;
	bus_dma_tag_t		dmat;
	bus_dmamap_t		dmamap;
	vm_memattr_t		memattr;
	bool			extmem;		/* the driver's allocator gave the memory (vbq_alloc_mem) */
	void			(*mem_free)(struct vbuf *);	/* gives extmem memory back */
	off_t			offset;		/* mmap offset handed to userland */

	/* the request this buffer is bound to (media request API), or NULL */
	struct vreq		*req;		/* while bound and not yet dequeued */
	struct vreq		*reqlist;	/* the request whose list holds it (owns a reference) */
	TAILQ_ENTRY(vbuf)	reqentry;	/* that list */
	int			request_fd;	/* as userland gave it, reported back */
	bool			req_done;	/* counted as finished by the request */
	bool			prepared;	/* PREPARE_BUF was done and not yet used */

	void			*priv;		/* driver */
};

/* ---------------------------------------------------------------- vbq_if.m */
#include "vbq_if.h"

#define	VBQ_MAX_BUFFERS		32

/* Frame-type flags that travel with a buffer (and are copied by mem2mem). */
#define	VBQ_FRAME_TYPE_FLAGS \
	(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME)

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
	bool			pages;		/* buffers are separate pages, not one DMA segment */
	bool			dmabuf;		/* may import dmabufs: the driver only uses b->vaddr */
	bus_addr_t		dma_maxaddr;	/* highest address the device reaches; 0: no limit (DMA segment buffers) */
	uint32_t		memory;		/* V4L2_MEMORY_* of the allocated buffers, 0 if none */
	bool			is_output;	/* OUTPUT queue: userland fills it */
	bool			ts_copy;	/* timestamps are copied, not made */
	off_t			offset_base;	/* first mmap offset of this queue */
	bool			streaming;	/* STREAMON seen */
	bool			started;	/* start_streaming() was called */
	bool			error;		/* the driver reported a fatal error */
	off_t			next_offset;
	struct vfh		*owner;		/* fh that allocated the buffers */
	bool			supports_hold;	/* OUTPUT of a mem2mem: V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF works */
	bool			supports_requests;	/* buffers may be queued through requests */
	bool			requires_requests;	/* ... and may not be queued otherwise */
	bool			uses_requests;	/* a buffer was queued through a request */
	bool			uses_qbuf;	/* a buffer was queued directly */
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
/* Are any buffers allocated on the queue? */
bool vbq_has_buffers(struct vbq *);
/* A buffer by its timestamp in nanoseconds (as the stateless controls name references), or NULL. */
struct vbuf *vbq_find_timestamp(struct vbq *, uint64_t ns);

/* --------------------------------------------------------------- vdev_if.m */
#include "vdev_if.h"

#define	VEVENT_QUEUE_LEN	32

/* A subscription of one open file to one kind of event. */
struct vevent_sub {
	TAILQ_ENTRY(vevent_sub)	link;
	uint32_t		type;		/* V4L2_EVENT_* */
	uint32_t		id;		/* control id for V4L2_EVENT_CTRL */
	uint32_t		flags;		/* V4L2_EVENT_SUB_FL_* */
};

struct vdev {
	KOBJ_FIELDS;
	device_t		dev;		/* parent device, for the DMA tag */
	struct cdev		*cdev;
	int			unit;		/* /dev/video<unit> */
	uint32_t		device_caps;	/* V4L2_CAP_* of this node */
	/*
	 * Set before vdev_register() to have the node appear in a media device:
	 * one entity (two plus a processing one for a mem2mem node) and an
	 * interface for /dev/videoN. `entity_function` is MEDIA_ENT_F_* of the
	 * processing entity of a mem2mem node; a plain node is MEDIA_ENT_F_IO_V4L.
	 */
	struct vmedia		*mdev;
	bool			requests;	/* some queue of it takes requests (media device) */
	const char		*entity_name;
	uint32_t		entity_function;
	struct vmedia_entity	*entities;	/* created by vdev_register() */
	uint32_t		nent;
	struct vmedia_intf	*intf;
	struct sx		lock;		/* serialises ioctls */
	struct vbq		*queue;		/* shared queue of a simple device, or NULL */
	struct vctrl_handler	*ctrl_handler;	/* shared controls, or NULL */
	/*
	 * poll(2)/select(2) wait here. FreeBSD allows a thread two selrecord()s per
	 * scan, a mem2mem device has three sources (two queues and events), so every
	 * change that can make a file ready wakes this one, and a woken file looks
	 * again. kqueue has its own per-queue lists (rsel.si_note) and needs no such
	 * limit.
	 */
	struct selinfo		sel;
	TAILQ_HEAD(, vfh)	fhs;		/* protected by the core's global lock */
	u_int			nclosing;	/* file closes in progress */
	u_int			prio[V4L2_PRIORITY_RECORD + 1];	/* open files per priority */
	bool			registered;
	bool			dying;
};

/* Per-open state. */
struct vfh {
	TAILQ_ENTRY(vfh)	link;
	struct vdev		*vdev;		/* NULL after the device went away */
	struct vbq		*capq;		/* the queues this file uses; NULL if */
	struct vbq		*outq;		/* the device has none of that kind */
	struct vctrl_handler	*ctrl_handler;
	void			*priv;		/* driver's per-open state */
	bool			detaching;	/* vdev_unregister() is taking it apart */
	enum v4l2_priority	prio;
	/* events: a ring of undelivered events and what this file asked for */
	struct mtx		evmtx;
	struct selinfo		*vsel;		/* the device's: wake it when an event arrives */
	TAILQ_HEAD(, vevent_sub) subs;
	struct v4l2_event	events[VEVENT_QUEUE_LEN];
	u_int			ev_head, ev_count;
	uint32_t		ev_seq;
};

/*
 * Queue an event for every open file subscribed to (type, id), except `src`
 * unless that file asked for feedback. For drivers: source changes and so on.
 */
void vdev_event_queue(struct vdev *, const struct v4l2_event *, struct vfh *src);

/*
 * Make the node /dev/video<N>. `v` must be zeroed and have its kobj class set
 * with kobj_init(); v->queue and v->ctrl_handler may be NULL.
 */
int vdev_register(struct vdev *v, device_t dev, uint32_t device_caps);
/* Remove the node, wait for ioctls in flight, and detach open files. */
void vdev_unregister(struct vdev *v);

/* ----------------------------------------------------------------- vmedia */

/*
 * A media device (/dev/mediaN): the graph of entities, pads and links of one
 * piece of hardware, plus the request API through which controls and buffers
 * are applied together.
 */
struct vmedia_pad {
	struct vmedia_entity	*ent;
	uint32_t		id;
	uint32_t		index;
	uint32_t		flags;		/* MEDIA_PAD_FL_* */
};

struct vmedia_entity {
	TAILQ_ENTRY(vmedia_entity) link;
	uint32_t		id;
	char			name[64];
	uint32_t		function;	/* MEDIA_ENT_F_* */
	uint32_t		flags;		/* MEDIA_ENT_FL_* */
	uint32_t		npads;
	struct vmedia_pad	*pads;
	struct vmedia_intf	*intf;		/* the node that opens it, if any */
};

struct vmedia_intf {
	TAILQ_ENTRY(vmedia_intf) link;
	uint32_t		id;
	uint32_t		type;		/* MEDIA_INTF_T_* */
	uint32_t		major, minor;
};

struct vmedia_link {
	TAILQ_ENTRY(vmedia_link) link;
	uint32_t		id;
	uint32_t		flags;		/* MEDIA_LNK_FL_* */
	/* a data link joins two pads, an interface link an interface and an entity */
	struct vmedia_pad	*src, *sink;
	struct vmedia_intf	*intf;
	struct vmedia_entity	*ent;
};

struct vmedia {
	struct cdev		*cdev;
	int			unit;		/* /dev/media<unit> */
	char			driver[16], model[32], serial[40], bus_info[32];
	uint32_t		hw_revision, driver_version;
	struct sx		lock;		/* the graph */
	TAILQ_HEAD(, vmedia_entity) entities;
	TAILQ_HEAD(, vmedia_intf) intfs;
	TAILQ_HEAD(, vmedia_link) links;
	uint32_t		next_ent, next_pad, next_link, next_intf;
	uint64_t		topology_version;
	uint64_t		serial_no;	/* identifies this device to its requests */
	u_int			nopen;
	bool			registered;
	bool			request_capable;	/* a queue of it takes requests */
};

void vmedia_init(struct vmedia *, const char *driver, const char *model,
    const char *bus_info, uint32_t hw_revision);
/* Entity with `npads` pads; pad_flags[i] is MEDIA_PAD_FL_* of pad i. */
int vmedia_entity_add(struct vmedia *, struct vmedia_entity *, const char *name,
    uint32_t function, uint32_t npads, const uint32_t *pad_flags);
void vmedia_entity_remove(struct vmedia *, struct vmedia_entity *);
int vmedia_link_pads(struct vmedia *, struct vmedia_entity *src, uint32_t src_pad,
    struct vmedia_entity *sink, uint32_t sink_pad, uint32_t flags);
int vmedia_intf_add(struct vmedia *, struct vmedia_intf *, uint32_t type,
    uint32_t major, uint32_t minor);
void vmedia_intf_remove(struct vmedia *, struct vmedia_intf *);
int vmedia_link_intf(struct vmedia *, struct vmedia_intf *, struct vmedia_entity *,
    uint32_t flags);
/* Creates /dev/media<unit>. */
int vmedia_register(struct vmedia *);
void vmedia_unregister(struct vmedia *);

/* The request a buffer is bound to, for a driver that needs to look inside it. */
static inline struct vreq *
vbuf_request(const struct vbuf *b)
{

	return (b->req);
}

#endif /* _KERNEL */

#endif /* _DEV_MEDIA_MEDIA_H_ */
