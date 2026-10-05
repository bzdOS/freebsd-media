/* SPDX-License-Identifier: BSD-2-Clause */
/* Calls between the media core's own files; drivers do not include this. */
#ifndef _DEV_MEDIA_MEDIA_PRIV_H_
#define	_DEV_MEDIA_MEDIA_PRIV_H_

#include <sys/malloc.h>

#include <dev/media/media.h>

/* Buffers still alive anywhere (a mapping keeps one alive): no module unload. */
u_int vbq_live_buffers(void);

/* ---- vbq.c: called by vdev.c with v->lock held, except dqbuf/poll/kqfilter */
int  vbq_reqbufs(struct vbq *, struct vfh *, struct v4l2_requestbuffers *);
int  vbq_querybuf(struct vbq *, struct v4l2_buffer *);
int  vbq_qbuf(struct vbq *, struct v4l2_buffer *);
int  vbq_prepare_buf(struct vbq *, struct v4l2_buffer *);
int  vbq_dqbuf(struct vbq *, struct v4l2_buffer *, bool nonblock);
int  vbq_expbuf(struct vbq *, struct v4l2_exportbuffer *);
int  vbq_streamon(struct vbq *, enum v4l2_buf_type);
int  vbq_streamoff(struct vbq *, enum v4l2_buf_type);
int  vbq_mmap_single(struct vbq *, vm_ooffset_t *, vm_size_t, vm_object_t *, int);
int  vbq_poll(struct vbq *, int events);			/* readiness only */
int  vbq_poll_events(struct vbq *);
bool vbq_poll_usable(struct vbq *);
int  vbq_kqfilter(struct vbq *, struct knote *);
/* The fh is going away: drop what it owns. */
void vbq_release(struct vbq *, struct vfh *);
/* The device is going away: stop and free everything, whoever owns it. */
void vbq_shutdown(struct vbq *);
bool vbq_busy_for(struct vbq *, struct vfh *);	/* owned by another fh */

/* ---- vevent.c (per-file event queues; vdev.c calls these) */
void vevent_fh_init(struct vfh *);
void vevent_fh_destroy(struct vfh *);
int  vevent_subscribe(struct vfh *, struct vctrl_handler *, struct v4l2_event_subscription *);
int  vevent_unsubscribe(struct vfh *, const struct v4l2_event_subscription *);
int  vevent_dequeue(struct vfh *, struct v4l2_event *, bool nonblock);
int  vevent_poll(struct vfh *, int events);		/* readiness only */
void vevent_deliver(struct vfh *, const struct v4l2_event *, struct vfh *src);
void vevent_ctrl_changed(struct vdev *, struct vctrl *, uint32_t changes, struct vfh *src);

/* ---- vbq.c, continued */
int  vbq_create_bufs(struct vbq *, struct vfh *, struct v4l2_create_buffers *, size_t sizeimage);

/* ---- vctrl.c */
int  vctrl_ioctl_queryctrl(struct vctrl_handler *, struct v4l2_queryctrl *);
int  vctrl_ioctl_query_ext_ctrl(struct vctrl_handler *, struct v4l2_query_ext_ctrl *);
int  vctrl_ioctl_querymenu(struct vctrl_handler *, struct v4l2_querymenu *);
int  vctrl_ioctl_g_ctrl(struct vctrl_handler *, struct v4l2_control *);
int  vctrl_ioctl_s_ctrl(struct vdev *, struct vfh *, struct v4l2_control *);
int  vctrl_ioctl_ext_ctrls(struct vdev *, struct vfh *, u_long cmd, struct v4l2_ext_controls *);

/* ---- vbq.c: buffer references, for the request API */
void vbuf_hold(struct vbuf *);
void vbuf_put(struct vbuf *);

/* ---- vreq.c: the media request API */

MALLOC_DECLARE(M_VREQ);

enum vreq_state {
	VREQ_IDLE,		/* collecting buffers and control values */
	VREQ_QUEUED,		/* handed to the drivers */
	VREQ_COMPLETE,		/* every buffer is done */
};

/* A control value held by a request: set by userland, or the state once applied. */
struct vreq_ctrl {
	TAILQ_ENTRY(vreq_ctrl)	link;
	uint32_t		id;
	bool			set;		/* by userland, not a snapshot */
	int64_t			val;
	uint8_t			*data;		/* payload controls */
	uint32_t		size;
};

struct vreq {
	u_int			refs;		/* the descriptor, and each bound buffer */
	uint64_t		media_serial;	/* the media device it belongs to */
	struct mtx		mtx;		/* state, pending, readiness */
	enum vreq_state		state;
	u_int			pending;	/* bound buffers not done yet */
	struct selinfo		sel;
	TAILQ_HEAD(, vbuf)	bufs;		/* under vreq_lock() */
	struct sx		clk;		/* ctrls, hdl */
	TAILQ_HEAD(, vreq_ctrl)	ctrls;
	struct vctrl_handler	*hdl;		/* the handler the controls are for */
	TAILQ_ENTRY(vreq)	gentry;
};

/*
 * vreq_lock() orders everything that changes which buffers a request holds, and
 * a request's state from IDLE: binding, QUEUE, REINIT, close, and a queue
 * letting go of its buffers. The control ioctls take it shared. The completion
 * path (vbq_buf_done) and a driver's job never do, so stop_streaming() may wait
 * for a job while a queue holds it.
 */
void vreq_lock(void);
void vreq_unlock(void);
void vreq_slock(void);
void vreq_sunlock(void);

int  vreq_alloc(struct vmedia *, struct thread *, int *fdp);
/* Look the descriptor up: EINVAL unless it is a request of media device `serial`. */
int  vreq_from_fd(int fd, uint64_t serial, struct thread *, struct vreq **);
void vreq_put(struct vreq *);
u_int vreq_live(void);
/* vreq_lock() held: bind b (DEQUEUED/PREPARED, q->mtx held) to an idle request. */
int  vreq_bind(struct vreq *, struct vbuf *);
/* vreq_lock() held: b leaves its request, if it is in one. The caller clears b->req. */
void vreq_unbind(struct vbuf *);
/* b finished (done, or taken back): no lock needed beyond the caller's reference. */
void vreq_buf_finished(struct vreq *, struct vbuf *);
/* A control handler is going away. */
void vreq_handler_gone(struct vctrl_handler *);
/* Control values of a request (vreq_slock() held; see vctrl.c). */
struct vreq_ctrl *vreq_ctrl_find(struct vreq *, uint32_t id);
struct vreq_ctrl *vreq_ctrl_add(struct vreq *, uint32_t id);
bool vreq_is_state(struct vreq *, enum vreq_state);
void vmedia_core_init(void);
void vmedia_core_fini(void);
void vreq_init(void);
int  vreq_fini(void);
/* vbq.c: a request was queued; give its buffer to the driver (vreq_lock() held). */
void vbq_request_queue_buf(struct vbq *, struct vbuf *);

#endif
