/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The /dev/videoN node: ioctl dispatch and per-open state.
 *
 * Queues belong to open files. A simple device has one shared queue (v->queue)
 * that every file uses, with the file that allocated its buffers owning it. A
 * mem2mem device gives each file its own pair of queues from its fh_open
 * method. Either way the ioctls find the queue through the file and the buffer
 * type in the request: vfh_queue().
 *
 * Lifetime of an open file. struct vfh is allocated at open and freed by the
 * cdevpriv destructor when the last reference to the file goes. It points at
 * the device, but a driver may detach with files still open. vdev_unregister()
 * drains the device's ioctls (destroy_dev), then takes every file apart itself:
 * it unlinks the file under vdev_mtx and marks it `detaching`, stops its queues
 * and calls fh_close outside the lock. A destructor that runs meanwhile waits
 * for `detaching` to clear; one that got there first is waited for through
 * v->nclosing. After that the destructor only frees the file.
 *
 * Locking: v->lock serialises ioctls; queue state has its own lock (vbq.c).
 * DQBUF and DQEVENT are the ioctls that run without v->lock, because they
 * sleep.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/condvar.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VDEV, "vdev", "V4L2 video devices");

/* Reported in v4l2_capability.version; the V4L2 API level we follow. */
#define	VDEV_KERNEL_VERSION	((6 << 16) | (12 << 8) | 0)
#define	VDEV_MAX_UNITS		64

static struct mtx vdev_mtx;		/* fh->vdev, v->fhs, v->nclosing, fh->detaching */
static struct cv vdev_cv;		/* nclosing or detaching changed */
static struct unrhdr *vdev_units;
static u_int vdev_count;

static d_open_t		vdev_open;
static d_ioctl_t	vdev_ioctl;
static d_poll_t		vdev_poll;
static d_kqfilter_t	vdev_kqfilter;
static d_mmap_single_t	vdev_mmap_single;
static d_read_t		vdev_rw;
static d_write_t	vdev_rw_w;

static struct cdevsw vdev_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"video",
	.d_open =	vdev_open,
	.d_ioctl =	vdev_ioctl,
	.d_poll =	vdev_poll,
	.d_kqfilter =	vdev_kqfilter,
	.d_mmap_single = vdev_mmap_single,
	.d_read =	vdev_rw,
	.d_write =	vdev_rw_w,
};

/* read(2)/write(2) I/O is not supported (V4L2_CAP_READWRITE is never set). */
static int
vdev_rw(struct cdev *dev, struct uio *uio, int ioflag)
{

	return (EINVAL);
}

static int
vdev_rw_w(struct cdev *dev, struct uio *uio, int ioflag)
{

	return (EINVAL);
}

/* The queue of this file for a buffer type, or NULL. */
static struct vbq *
vfh_queue(struct vfh *fh, uint32_t type)
{

	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return (fh->capq);
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return (fh->outq);
	}
	return (NULL);
}

/* ------------------------------------------------------------ open / close */

/*
 * Stop and free what a file owns, then let the driver release its per-open
 * state. The shared queue is only let go if this file owns it; a queue made
 * for the file is shut down outright (the driver destroys it in fh_close).
 */
static void
vfh_release(struct vdev *v, struct vfh *fh)
{
	struct vbq *qs[2] = { fh->capq, fh->outq };
	int i;

	sx_xlock(&v->lock);
	for (i = 0; i < 2; i++) {
		if (qs[i] == NULL)
			continue;
		if (qs[i] == v->queue)
			vbq_release(qs[i], fh);
		else
			vbq_shutdown(qs[i]);
	}
	VDEV_FH_CLOSE(v, fh);
	fh->capq = fh->outq = NULL;
	sx_xunlock(&v->lock);
}

static void
vdev_fh_dtor(void *arg)
{
	struct vfh *fh = arg;
	struct vdev *v;

	mtx_lock(&vdev_mtx);
	while (fh->detaching)
		cv_wait(&vdev_cv, &vdev_mtx);	/* vdev_unregister() has us */
	v = fh->vdev;
	if (v != NULL) {
		TAILQ_REMOVE(&v->fhs, fh, link);
		fh->vdev = NULL;
		v->prio[fh->prio]--;
		v->nclosing++;
	}
	mtx_unlock(&vdev_mtx);
	if (v != NULL) {
		vfh_release(v, fh);
		mtx_lock(&vdev_mtx);
		if (--v->nclosing == 0)
			cv_broadcast(&vdev_cv);
		mtx_unlock(&vdev_mtx);
	}
	vevent_fh_destroy(fh);
	free(fh, M_VDEV);
}

static int
vdev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	int error;

	if (v == NULL)
		return (ENXIO);
	fh = malloc(sizeof(*fh), M_VDEV, M_WAITOK | M_ZERO);
	vevent_fh_init(fh);
	fh->prio = V4L2_PRIORITY_DEFAULT;
	fh->vdev = v;
	fh->vsel = &v->sel;
	/* Start from the device's shared queue and controls, if it has them. */
	fh->ctrl_handler = v->ctrl_handler;
	if (v->queue != NULL) {
		if (v->queue->is_output)
			fh->outq = v->queue;
		else
			fh->capq = v->queue;
	}
	error = VDEV_FH_OPEN(v, fh);
	if (error != 0)
		goto fail;
	/* A handler of the file's own, made by the driver, belongs to this device too. */
	if (fh->ctrl_handler != NULL && fh->ctrl_handler->vdev == NULL)
		fh->ctrl_handler->vdev = v;

	mtx_lock(&vdev_mtx);
	if (v->dying) {
		mtx_unlock(&vdev_mtx);
		error = ENXIO;
		goto closeit;
	}
	TAILQ_INSERT_TAIL(&v->fhs, fh, link);
	v->prio[fh->prio]++;
	mtx_unlock(&vdev_mtx);
	error = devfs_set_cdevpriv(fh, vdev_fh_dtor);
	if (error != 0) {
		/* The destructor is not going to run for fh: undo by hand. */
		mtx_lock(&vdev_mtx);
		TAILQ_REMOVE(&v->fhs, fh, link);
		v->prio[fh->prio]--;
		mtx_unlock(&vdev_mtx);
		goto closeit;
	}
	return (0);
closeit:
	vfh_release(v, fh);
fail:
	vevent_fh_destroy(fh);
	free(fh, M_VDEV);
	return (error);
}

/* -------------------------------------------------- priority and events */

/* The highest priority any open file has asked for. */
static uint32_t
vdev_prio_max(struct vdev *v)
{
	int p;

	mtx_lock(&vdev_mtx);
	for (p = V4L2_PRIORITY_RECORD; p > V4L2_PRIORITY_UNSET; p--)
		if (v->prio[p] != 0)
			break;
	mtx_unlock(&vdev_mtx);
	return (p);
}

/* Ioctls that change device state need at least the highest priority. */
static bool
vdev_needs_prio(u_long cmd)
{

	switch (cmd) {
	case VIDIOC_S_FMT:
	case VIDIOC_S_PARM:
	case VIDIOC_S_INPUT:
	case VIDIOC_S_CTRL:
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_REQBUFS:
	case VIDIOC_CREATE_BUFS:
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF:
	case VIDIOC_S_PRIORITY:
		return (true);
	}
	return (false);
}

static int
vdev_s_priority(struct vdev *v, struct vfh *fh, uint32_t np)
{

	if (np < V4L2_PRIORITY_BACKGROUND || np > V4L2_PRIORITY_RECORD)
		return (EINVAL);
	mtx_lock(&vdev_mtx);
	v->prio[fh->prio]--;
	v->prio[np]++;
	fh->prio = np;
	mtx_unlock(&vdev_mtx);
	return (0);
}

void
vdev_event_queue(struct vdev *v, const struct v4l2_event *ev, struct vfh *src)
{
	struct vfh *fh;

	mtx_lock(&vdev_mtx);
	TAILQ_FOREACH(fh, &v->fhs, link)
		vevent_deliver(fh, ev, src);
	mtx_unlock(&vdev_mtx);
}

/* ------------------------------------------------------------------ ioctls */

/*
 * FreeBSD copies an ioctl's result out to the caller only when the handler
 * returns 0 (kern_ioctl). V4L2 returns data with some errors: VIDIOC_*_EXT_CTRLS
 * report error_idx and the values read so far. The handler only sees the kernel
 * copy, so for those ioctls we copy it out ourselves, to the address the
 * ioctl(2) call was made with (the third syscall argument). Anything that is
 * not that syscall is left alone.
 */
static void
vdev_copyout_on_error(struct thread *td, const void *data, size_t len)
{

	if (td->td_sa.code != SYS_ioctl)
		return;
	(void)copyout(data, (void *)td->td_sa.args[2], len);
}

static int
vdev_querycap(struct vdev *v, struct vfh *fh, struct v4l2_capability *cap)
{
	int error;

	memset(cap, 0, sizeof(*cap));
	error = VDEV_QUERYCAP(v, fh, cap);
	if (error != 0)
		return (error);
	cap->version = VDEV_KERNEL_VERSION;
	cap->device_caps = v->device_caps;
	cap->capabilities = v->device_caps | V4L2_CAP_DEVICE_CAPS;
	memset(cap->reserved, 0, sizeof(cap->reserved));
	return (0);
}

static int
vdev_enum_fmt(struct vdev *v, struct vfh *fh, struct v4l2_fmtdesc *f)
{
	uint32_t index = f->index, type = f->type;

	if (vfh_queue(fh, type) == NULL || f->mbus_code != 0)
		return (EINVAL);
	memset(f, 0, sizeof(*f));
	f->index = index;
	f->type = type;
	return (VDEV_ENUM_FMT(v, fh, f));
}

/* TRY_FMT and S_FMT: the driver sees only the part of the union it knows. */
static int
vdev_fmt(struct vdev *v, struct vfh *fh, u_long cmd, struct v4l2_format *f)
{
	struct v4l2_pix_format pix;
	struct vbq *q = vfh_queue(fh, f->type);

	if (q == NULL)
		return (EINVAL);
	switch (cmd) {
	case VIDIOC_G_FMT:
		memset(&f->fmt, 0, sizeof(f->fmt));
		return (VDEV_G_FMT(v, fh, f));
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT:
		if (cmd == VIDIOC_S_FMT &&
		    (vbq_busy_for(q, fh) || vbq_has_buffers(q)))
			return (EBUSY);
		pix = f->fmt.pix;
		memset(&f->fmt, 0, sizeof(f->fmt));
		f->fmt.pix = pix;
		f->fmt.pix.priv = 0;
		return (cmd == VIDIOC_S_FMT ? VDEV_S_FMT(v, fh, f) :
		    VDEV_TRY_FMT(v, fh, f));
	}
	return (ENOTTY);
}

/* Inputs: a capture device has one fixed camera input; a mem2mem device none. */
static bool
vdev_has_inputs(struct vdev *v)
{

	return ((v->device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0);
}

static int
vdev_enum_input(struct v4l2_input *in)
{
	uint32_t index = in->index;

	if (index != 0)
		return (EINVAL);
	memset(in, 0, sizeof(*in));
	in->index = 0;
	strlcpy((char *)in->name, "Camera 0", sizeof(in->name));
	in->type = V4L2_INPUT_TYPE_CAMERA;
	return (0);
}

static int
vdev_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	struct vbq *q;
	int error;

	error = devfs_get_cdevpriv((void **)&fh);
	if (error != 0)
		return (error);
	if (v == NULL || fh->vdev == NULL)
		return (ENODEV);

	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		return (0);
	case VIDIOC_DQBUF:
		/* No v->lock: this one sleeps (see the top of the file). */
		if (fh->capq == NULL && fh->outq == NULL)
			return (ENOTTY);
		q = vfh_queue(fh, ((struct v4l2_buffer *)data)->type);
		if (q == NULL)
			return (EINVAL);
		if (vbq_busy_for(q, fh))
			return (EBUSY);
		return (vbq_dqbuf(q, (struct v4l2_buffer *)data,
		    (fflag & O_NONBLOCK) != 0));
	case VIDIOC_DQEVENT:
		return (vevent_dequeue(fh, (struct v4l2_event *)data,
		    (fflag & O_NONBLOCK) != 0));
	}

	sx_xlock(&v->lock);
	if (v->dying) {
		sx_xunlock(&v->lock);
		return (ENODEV);
	}
	if (vdev_needs_prio(cmd) && fh->prio < vdev_prio_max(v)) {
		sx_xunlock(&v->lock);
		return (EBUSY);
	}
	switch (cmd) {
	case VIDIOC_QUERYCAP:
		error = vdev_querycap(v, fh, (struct v4l2_capability *)data);
		break;
	case VIDIOC_ENUM_FMT:
		error = vdev_enum_fmt(v, fh, (struct v4l2_fmtdesc *)data);
		break;
	case VIDIOC_G_FMT:
	case VIDIOC_S_FMT:
	case VIDIOC_TRY_FMT:
		error = vdev_fmt(v, fh, cmd, (struct v4l2_format *)data);
		break;
	case VIDIOC_ENUM_FRAMESIZES:
		error = VDEV_ENUM_FRAMESIZES(v, fh, (struct v4l2_frmsizeenum *)data);
		break;
	case VIDIOC_ENUM_FRAMEINTERVALS:
		error = VDEV_ENUM_FRAMEINTERVALS(v, fh, (struct v4l2_frmivalenum *)data);
		break;
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *p = (struct v4l2_streamparm *)data;

		/* A mem2mem device has no frame interval to set. */
		if ((v->device_caps & V4L2_CAP_VIDEO_M2M) != 0) {
			error = ENOTTY;
			break;
		}
		q = vfh_queue(fh, p->type);
		if (q == NULL)
			error = EINVAL;
		else if (cmd == VIDIOC_S_PARM &&
		    (vbq_busy_for(q, fh) || vbq_is_streaming(q)))
			error = EBUSY;
		else
			error = (cmd == VIDIOC_G_PARM) ? VDEV_G_PARM(v, fh, p) :
			    VDEV_S_PARM(v, fh, p);
		break;
	}
	case VIDIOC_ENUMINPUT:
		error = vdev_has_inputs(v) ? vdev_enum_input((struct v4l2_input *)data) :
		    ENOTTY;
		break;
	case VIDIOC_G_INPUT:
		if (!vdev_has_inputs(v)) {
			error = ENOTTY;
			break;
		}
		*(int *)data = 0;
		error = 0;
		break;
	case VIDIOC_S_INPUT:
		if (!vdev_has_inputs(v))
			error = ENOTTY;
		else
			error = (*(int *)data == 0) ? 0 : EINVAL;
		break;

	case VIDIOC_REQBUFS:
	case VIDIOC_CREATE_BUFS:
	case VIDIOC_QUERYBUF:
	case VIDIOC_QBUF:
	case VIDIOC_PREPARE_BUF:
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF: {
		uint32_t type;

		if (fh->capq == NULL && fh->outq == NULL) {
			error = ENOTTY;
			break;
		}
		switch (cmd) {
		case VIDIOC_REQBUFS:
			type = ((struct v4l2_requestbuffers *)data)->type;
			break;
		case VIDIOC_CREATE_BUFS:
			type = ((struct v4l2_create_buffers *)data)->format.type;
			break;
		case VIDIOC_STREAMON:
		case VIDIOC_STREAMOFF:
			type = *(uint32_t *)data;
			break;
		default:
			type = ((struct v4l2_buffer *)data)->type;
			break;
		}
		q = vfh_queue(fh, type);
		if (q == NULL) {
			error = EINVAL;
			break;
		}
		if (cmd != VIDIOC_QUERYBUF && vbq_busy_for(q, fh)) {
			error = EBUSY;
			break;
		}
		switch (cmd) {
		case VIDIOC_REQBUFS:
			error = vbq_reqbufs(q, fh, (struct v4l2_requestbuffers *)data);
			break;
		case VIDIOC_CREATE_BUFS: {
			struct v4l2_create_buffers *cb = (struct v4l2_create_buffers *)data;

			error = vbq_create_bufs(q, fh, cb, cb->format.fmt.pix.sizeimage);
			break;
		}
		case VIDIOC_QUERYBUF:
			error = vbq_querybuf(q, (struct v4l2_buffer *)data);
			break;
		case VIDIOC_QBUF:
			error = vbq_qbuf(q, (struct v4l2_buffer *)data);
			break;
		case VIDIOC_PREPARE_BUF:
			error = vbq_prepare_buf(q, (struct v4l2_buffer *)data);
			break;
		case VIDIOC_STREAMON:
			error = vbq_streamon(q, type);
			break;
		default:
			error = vbq_streamoff(q, type);
			break;
		}
		break;
	}

	case VIDIOC_EXPBUF: {
		struct v4l2_exportbuffer *exp = (struct v4l2_exportbuffer *)data;

		if (fh->capq == NULL && fh->outq == NULL) {
			error = ENOTTY;
			break;
		}
		q = vfh_queue(fh, exp->type);
		error = (q == NULL) ? EINVAL : vbq_expbuf(q, exp);
		break;
	}

	case VIDIOC_QUERYCTRL:
		error = vctrl_ioctl_queryctrl(fh->ctrl_handler, (struct v4l2_queryctrl *)data);
		break;
	case VIDIOC_QUERY_EXT_CTRL:
		error = vctrl_ioctl_query_ext_ctrl(fh->ctrl_handler, (struct v4l2_query_ext_ctrl *)data);
		break;
	case VIDIOC_QUERYMENU:
		error = vctrl_ioctl_querymenu(fh->ctrl_handler, (struct v4l2_querymenu *)data);
		break;
	case VIDIOC_G_CTRL:
		error = vctrl_ioctl_g_ctrl(fh->ctrl_handler, (struct v4l2_control *)data);
		break;
	case VIDIOC_S_CTRL:
		error = vctrl_ioctl_s_ctrl(v, fh, (struct v4l2_control *)data);
		break;
	case VIDIOC_G_EXT_CTRLS:
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_TRY_EXT_CTRLS:
		error = vctrl_ioctl_ext_ctrls(v, fh, cmd, (struct v4l2_ext_controls *)data);
		if (error != 0)
			vdev_copyout_on_error(td, data, sizeof(struct v4l2_ext_controls));
		break;

	case VIDIOC_G_PRIORITY:
		*(uint32_t *)data = vdev_prio_max(v);
		error = 0;
		break;
	case VIDIOC_S_PRIORITY:
		error = vdev_s_priority(v, fh, *(uint32_t *)data);
		break;
	case VIDIOC_SUBSCRIBE_EVENT:
		error = vevent_subscribe(fh, fh->ctrl_handler,
		    (struct v4l2_event_subscription *)data);
		break;
	case VIDIOC_UNSUBSCRIBE_EVENT:
		error = vevent_unsubscribe(fh,
		    (struct v4l2_event_subscription *)data);
		break;

	default:
		error = ENOTTY;
		break;
	}
	sx_xunlock(&v->lock);
	return (error);
}

/* ------------------------------------------------- poll, kqueue and mmap */

static int
vdev_poll(struct cdev *dev, int events, struct thread *td)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	int revents, asked = 0;

	if (v == NULL || devfs_get_cdevpriv((void **)&fh) != 0)
		return (POLLERR);
	/*
	 * Record first, check after: a change between the two then finds us
	 * already waiting instead of being missed. Once per call (see v->sel).
	 */
	selrecord(td, &v->sel);
	revents = vevent_poll(fh, events);
	/*
	 * Buffers: a queue contributes the events it is about when a buffer is
	 * done. POLLERR means nothing can ever become ready: for one queue that is
	 * the queue being unusable, for a mem2mem pair both of them (one of the two
	 * idling is normal).
	 */
	if (fh->capq != NULL)
		asked |= events & vbq_poll_events(fh->capq);
	if (fh->outq != NULL)
		asked |= events & vbq_poll_events(fh->outq);
	if (asked != 0) {
		if (fh->capq != NULL)
			revents |= vbq_poll(fh->capq, events);
		if (fh->outq != NULL)
			revents |= vbq_poll(fh->outq, events);
		if (revents == 0 &&
		    !(fh->capq != NULL && vbq_poll_usable(fh->capq)) &&
		    !(fh->outq != NULL && vbq_poll_usable(fh->outq)))
			revents = POLLERR;
	}
	if (fh->capq == NULL && fh->outq == NULL &&
	    (events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM)) != 0)
		revents |= POLLERR;
	return (revents);
}

static int
vdev_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	struct vbq *q;

	if (v == NULL || devfs_get_cdevpriv((void **)&fh) != 0)
		return (EINVAL);
	q = (kn->kn_filter == EVFILT_READ) ? fh->capq :
	    (kn->kn_filter == EVFILT_WRITE) ? fh->outq : NULL;
	if (q == NULL)
		return (EINVAL);
	return (vbq_kqfilter(q, kn));
}

static int
vdev_mmap_single(struct cdev *dev, vm_ooffset_t *offset, vm_size_t size,
    vm_object_t *object, int nprot)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	int error = EINVAL;

	if (v == NULL || devfs_get_cdevpriv((void **)&fh) != 0)
		return (ENODEV);
	/* Each queue's offsets live in their own range, so at most one matches. */
	if (fh->capq != NULL)
		error = vbq_mmap_single(fh->capq, offset, size, object, nprot);
	if (error == EINVAL && fh->outq != NULL)
		error = vbq_mmap_single(fh->outq, offset, size, object, nprot);
	return (error);
}

/* -------------------------------------------------- register / unregister */

/*
 * The node's place in its media device: a capture or output node is one entity
 * with one pad; a mem2mem node is a source entity feeding a processing entity
 * feeding a sink entity, the way Linux describes it. The interface is the
 * /dev/video node, linked to the (first) entity that opens it.
 */
static int
vdev_media_add(struct vdev *v)
{
	struct vmedia *m = v->mdev;
	struct vmedia_entity *e;
	const char *base = v->entity_name != NULL ? v->entity_name : "video";
	char name[64];
	uint32_t pf[2];
	dev_t ud = dev2udev(v->cdev);
	bool m2m = (v->device_caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
	bool out = (v->device_caps & (V4L2_CAP_VIDEO_OUTPUT |
	    V4L2_CAP_VIDEO_OUTPUT_MPLANE)) != 0;

	v->intf = malloc(sizeof(*v->intf), M_TEMP, M_WAITOK | M_ZERO);
	v->nent = m2m ? 3 : 1;
	v->entities = malloc(v->nent * sizeof(*v->entities), M_TEMP, M_WAITOK | M_ZERO);
	vmedia_intf_add(m, v->intf, MEDIA_INTF_T_V4L_VIDEO, major(ud), minor(ud));
	if (!m2m) {
		pf[0] = out ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;
		vmedia_entity_add(m, &v->entities[0], base, MEDIA_ENT_F_IO_V4L, 1, pf);
		vmedia_link_intf(m, v->intf, &v->entities[0],
		    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		return (0);
	}
	snprintf(name, sizeof(name), "%s-source", base);
	pf[0] = MEDIA_PAD_FL_SOURCE;
	vmedia_entity_add(m, &v->entities[0], name, MEDIA_ENT_F_IO_V4L, 1, pf);
	snprintf(name, sizeof(name), "%s-proc", base);
	pf[0] = MEDIA_PAD_FL_SINK;
	pf[1] = MEDIA_PAD_FL_SOURCE;
	vmedia_entity_add(m, &v->entities[1], name,
	    v->entity_function != 0 ? v->entity_function :
	    MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER, 2, pf);
	snprintf(name, sizeof(name), "%s-sink", base);
	pf[0] = MEDIA_PAD_FL_SINK;
	vmedia_entity_add(m, &v->entities[2], name, MEDIA_ENT_F_IO_V4L, 1, pf);
	e = v->entities;
	vmedia_link_pads(m, &e[0], 0, &e[1], 0, MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	vmedia_link_pads(m, &e[1], 1, &e[2], 0, MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	vmedia_link_intf(m, v->intf, &e[0], MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	vmedia_link_intf(m, v->intf, &e[2], MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	return (0);
}

static void
vdev_media_remove(struct vdev *v)
{
	uint32_t i;

	if (v->intf == NULL)
		return;
	vmedia_intf_remove(v->mdev, v->intf);
	for (i = 0; i < v->nent; i++)
		vmedia_entity_remove(v->mdev, &v->entities[i]);
	free(v->entities, M_TEMP);
	free(v->intf, M_TEMP);
	v->entities = NULL;
	v->intf = NULL;
	v->nent = 0;
}

int
vdev_register(struct vdev *v, device_t dev, uint32_t device_caps)
{
	struct make_dev_args args;
	int error, unit;

	v->dev = dev;
	v->device_caps = device_caps;
	TAILQ_INIT(&v->fhs);
	sx_init(&v->lock, "vdev");
	if (v->queue != NULL)
		v->queue->vdev = v;
	if (v->ctrl_handler != NULL)
		v->ctrl_handler->vdev = v;

	unit = alloc_unr(vdev_units);
	if (unit < 0) {
		sx_destroy(&v->lock);
		return (ENOSPC);
	}
	v->unit = unit;
	make_dev_args_init(&args);
	args.mda_devsw = &vdev_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_OPERATOR;
	args.mda_mode = 0660;
	args.mda_si_drv1 = v;
	error = make_dev_s(&args, &v->cdev, "video%d", unit);
	if (error != 0) {
		free_unr(vdev_units, unit);
		sx_destroy(&v->lock);
		return (error);
	}
	mtx_lock(&vdev_mtx);
	vdev_count++;
	v->registered = true;
	mtx_unlock(&vdev_mtx);
	if (v->mdev != NULL) {
		if (v->requests)
			v->mdev->request_capable = true;
		vdev_media_add(v);
	}
	return (0);
}

void
vdev_unregister(struct vdev *v)
{
	struct vfh *fh;

	if (!v->registered)
		return;
	mtx_lock(&vdev_mtx);
	v->dying = true;
	mtx_unlock(&vdev_mtx);

	/* New opens are refused and ioctls/polls/mmaps in flight are drained. */
	destroy_dev(v->cdev);
	v->cdev = NULL;

	/* Take the open files apart; see the top of the file for the handshake. */
	mtx_lock(&vdev_mtx);
	while ((fh = TAILQ_FIRST(&v->fhs)) != NULL) {
		TAILQ_REMOVE(&v->fhs, fh, link);
		fh->vdev = NULL;
		fh->detaching = true;
		mtx_unlock(&vdev_mtx);
		vfh_release(v, fh);
		mtx_lock(&vdev_mtx);
		fh->detaching = false;
		cv_broadcast(&vdev_cv);
	}
	while (v->nclosing != 0)
		cv_wait(&vdev_cv, &vdev_mtx);
	v->registered = false;
	vdev_count--;
	mtx_unlock(&vdev_mtx);

	if (v->queue != NULL)
		vbq_shutdown(v->queue);
	vdev_media_remove(v);
	seldrain(&v->sel);
	free_unr(vdev_units, v->unit);
	sx_destroy(&v->lock);
}

/* --------------------------------------------------------------- module */

static int
media_modevent(module_t mod, int what, void *arg)
{
	int error = 0;

	switch (what) {
	case MOD_LOAD:
		mtx_init(&vdev_mtx, "vdev global", NULL, MTX_DEF);
		cv_init(&vdev_cv, "vdevcl");
		vdev_units = new_unrhdr(0, VDEV_MAX_UNITS - 1, NULL);
		vmedia_core_init();
		vreq_init();
		break;
	case MOD_UNLOAD:
		/* Requests are files whose operations live here, buffers likewise. */
		if (vdev_count != 0 || vbq_live_buffers() != 0 || vreq_live() != 0) {
			error = EBUSY;
			break;
		}
		error = vreq_fini();
		if (error != 0)
			break;
		vmedia_core_fini();
		delete_unrhdr(vdev_units);
		cv_destroy(&vdev_cv);
		mtx_destroy(&vdev_mtx);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return (error);
}

static moduledata_t media_mod = {
	"media",
	media_modevent,
	NULL
};
DECLARE_MODULE(media, media_mod, SI_SUB_DRIVERS, SI_ORDER_FIRST);
MODULE_VERSION(media, 1);
MODULE_DEPEND(media, dmabuf_base, 1, 1, 1);
