/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The /dev/videoN node: ioctl dispatch and per-open state.
 *
 * Lifetime of an open file. struct vfh is allocated at open and freed by the
 * cdevpriv destructor when the last reference to the file goes. It points at
 * the device, but a driver may detach with files still open: vdev_unregister()
 * sets fh->vdev to NULL for all of them under vdev_mtx and waits for closes
 * already in progress, so a destructor either sees a live device (and keeps
 * it alive through vdev->nclosing) or none.
 *
 * Locking: v->lock serialises ioctls; queue state has its own lock (vbq.c).
 * DQBUF is the only ioctl that runs without v->lock, because it sleeps.
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

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VDEV, "vdev", "V4L2 video devices");

/* Reported in v4l2_capability.version; the V4L2 API level we follow. */
#define	MEDIA_API_VERSION	((6 << 16) | (12 << 8) | 0)
#define	VDEV_MAX_UNITS		64

static struct mtx vdev_mtx;		/* fh->vdev, v->fhs, v->nclosing */
static struct cv vdev_cv;		/* nclosing dropped to 0 */
static struct unrhdr *vdev_units;
static u_int vdev_count;

static d_open_t		vdev_open;
static d_ioctl_t	vdev_ioctl;
static d_poll_t		vdev_poll;
static d_kqfilter_t	vdev_kqfilter;
static d_mmap_single_t	vdev_mmap_single;

static struct cdevsw vdev_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"video",
	.d_open =	vdev_open,
	.d_ioctl =	vdev_ioctl,
	.d_poll =	vdev_poll,
	.d_kqfilter =	vdev_kqfilter,
	.d_mmap_single = vdev_mmap_single,
};

/* ------------------------------------------------------------ open / close */

static void
vdev_fh_dtor(void *arg)
{
	struct vfh *fh = arg;
	struct vdev *v;

	mtx_lock(&vdev_mtx);
	v = fh->vdev;
	if (v != NULL) {
		TAILQ_REMOVE(&v->fhs, fh, link);
		fh->vdev = NULL;
		v->nclosing++;
	}
	mtx_unlock(&vdev_mtx);
	if (v != NULL) {
		sx_xlock(&v->lock);
		if (v->queue != NULL)
			vbq_release(v->queue, fh);
		sx_xunlock(&v->lock);
		mtx_lock(&vdev_mtx);
		if (--v->nclosing == 0)
			cv_broadcast(&vdev_cv);
		mtx_unlock(&vdev_mtx);
	}
	free(fh, M_VDEV);
}

static int
vdev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct vdev *v = dev->si_drv1;
	struct vfh *fh;
	int error;

	fh = malloc(sizeof(*fh), M_VDEV, M_WAITOK | M_ZERO);
	mtx_lock(&vdev_mtx);
	if (v == NULL || v->dying) {
		mtx_unlock(&vdev_mtx);
		free(fh, M_VDEV);
		return (ENXIO);
	}
	fh->vdev = v;
	TAILQ_INSERT_TAIL(&v->fhs, fh, link);
	mtx_unlock(&vdev_mtx);
	error = devfs_set_cdevpriv(fh, vdev_fh_dtor);
	if (error != 0) {
		/* The destructor is not going to run for fh: undo by hand. */
		mtx_lock(&vdev_mtx);
		if (fh->vdev != NULL)
			TAILQ_REMOVE(&v->fhs, fh, link);
		mtx_unlock(&vdev_mtx);
		free(fh, M_VDEV);
	}
	return (error);
}

/* ------------------------------------------------------------------ ioctls */

static int
vdev_querycap(struct vdev *v, struct v4l2_capability *cap)
{
	int error;

	memset(cap, 0, sizeof(*cap));
	error = VDEV_QUERYCAP(v, cap);
	if (error != 0)
		return (error);
	cap->version = MEDIA_API_VERSION;
	cap->device_caps = v->device_caps;
	cap->capabilities = v->device_caps | V4L2_CAP_DEVICE_CAPS;
	memset(cap->reserved, 0, sizeof(cap->reserved));
	return (0);
}

static bool
vdev_type_ok(struct vdev *v, uint32_t type)
{

	return (v->queue != NULL && type == v->queue->type);
}

static int
vdev_enum_fmt(struct vdev *v, struct v4l2_fmtdesc *f)
{
	uint32_t index = f->index, type = f->type;

	if (!vdev_type_ok(v, type) || f->mbus_code != 0)
		return (EINVAL);
	memset(f, 0, sizeof(*f));
	f->index = index;
	f->type = type;
	return (VDEV_ENUM_FMT(v, f));
}

/* TRY_FMT and S_FMT: the driver sees only the part of the union it knows. */
static int
vdev_fmt(struct vdev *v, struct vfh *fh, u_long cmd, struct v4l2_format *f)
{
	struct v4l2_pix_format pix;
	uint32_t type = f->type;
	int error;

	if (!vdev_type_ok(v, type))
		return (EINVAL);
	switch (cmd) {
	case VIDIOC_G_FMT:
		memset(&f->fmt, 0, sizeof(f->fmt));
		return (VDEV_G_FMT(v, f));
	case VIDIOC_TRY_FMT:
	case VIDIOC_S_FMT:
		if (cmd == VIDIOC_S_FMT) {
			if (vbq_busy_for(v->queue, fh) || vbq_has_buffers(v->queue))
				return (EBUSY);
		}
		pix = f->fmt.pix;
		memset(&f->fmt, 0, sizeof(f->fmt));
		f->fmt.pix = pix;
		f->fmt.pix.priv = 0;
		error = (cmd == VIDIOC_S_FMT) ? VDEV_S_FMT(v, f) : VDEV_TRY_FMT(v, f);
		return (error);
	}
	return (ENOTTY);
}

static int
vdev_enum_input(struct vfh *fh, struct v4l2_input *in)
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
vdev_reqbufs(struct vdev *v, struct vfh *fh, struct v4l2_requestbuffers *rb)
{

	if (v->queue == NULL)
		return (ENOTTY);
	if (vbq_busy_for(v->queue, fh))
		return (EBUSY);
	return (vbq_reqbufs(v->queue, fh, rb));
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
	q = v->queue;

	switch (cmd) {
	case FIONBIO:
	case FIOASYNC:
		return (0);
	case VIDIOC_DQBUF:
		/* No v->lock: this one sleeps (see the top of the file). */
		if (q == NULL)
			return (ENOTTY);
		if (vbq_busy_for(q, fh))
			return (EBUSY);
		return (vbq_dqbuf(q, (struct v4l2_buffer *)data,
		    (fflag & O_NONBLOCK) != 0));
	}

	sx_xlock(&v->lock);
	if (v->dying) {
		sx_xunlock(&v->lock);
		return (ENODEV);
	}
	switch (cmd) {
	case VIDIOC_QUERYCAP:
		error = vdev_querycap(v, (struct v4l2_capability *)data);
		break;
	case VIDIOC_ENUM_FMT:
		error = vdev_enum_fmt(v, (struct v4l2_fmtdesc *)data);
		break;
	case VIDIOC_G_FMT:
	case VIDIOC_S_FMT:
	case VIDIOC_TRY_FMT:
		error = vdev_fmt(v, fh, cmd, (struct v4l2_format *)data);
		break;
	case VIDIOC_ENUM_FRAMESIZES:
		error = VDEV_ENUM_FRAMESIZES(v, (struct v4l2_frmsizeenum *)data);
		break;
	case VIDIOC_ENUM_FRAMEINTERVALS:
		error = VDEV_ENUM_FRAMEINTERVALS(v, (struct v4l2_frmivalenum *)data);
		break;
	case VIDIOC_G_PARM:
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *p = (struct v4l2_streamparm *)data;

		if (!vdev_type_ok(v, p->type))
			error = EINVAL;
		else if (cmd == VIDIOC_S_PARM &&
		    (vbq_busy_for(q, fh) || vbq_is_streaming(q)))
			error = EBUSY;
		else
			error = (cmd == VIDIOC_G_PARM) ? VDEV_G_PARM(v, p) :
			    VDEV_S_PARM(v, p);
		break;
	}
	case VIDIOC_ENUMINPUT:
		error = vdev_enum_input(fh, (struct v4l2_input *)data);
		break;
	case VIDIOC_G_INPUT:
		*(int *)data = 0;
		error = 0;
		break;
	case VIDIOC_S_INPUT:
		error = (*(int *)data == 0) ? 0 : EINVAL;
		break;

	case VIDIOC_REQBUFS:
		error = vdev_reqbufs(v, fh, (struct v4l2_requestbuffers *)data);
		break;
	case VIDIOC_QUERYBUF:
		error = (q == NULL) ? ENOTTY : vbq_querybuf(q, (struct v4l2_buffer *)data);
		break;
	case VIDIOC_QBUF:
	case VIDIOC_PREPARE_BUF:
		if (q == NULL)
			error = ENOTTY;
		else if (vbq_busy_for(q, fh))
			error = EBUSY;
		else if (cmd == VIDIOC_QBUF)
			error = vbq_qbuf(q, (struct v4l2_buffer *)data);
		else
			error = vbq_prepare_buf(q, (struct v4l2_buffer *)data);
		break;
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF:
		if (q == NULL)
			error = ENOTTY;
		else if (vbq_busy_for(q, fh))
			error = EBUSY;
		else if (cmd == VIDIOC_STREAMON)
			error = vbq_streamon(q, *(uint32_t *)data);
		else
			error = vbq_streamoff(q, *(uint32_t *)data);
		break;

	case VIDIOC_QUERYCTRL:
		error = vctrl_ioctl_queryctrl(v->ctrl_handler, (struct v4l2_queryctrl *)data);
		break;
	case VIDIOC_QUERY_EXT_CTRL:
		error = vctrl_ioctl_query_ext_ctrl(v->ctrl_handler, (struct v4l2_query_ext_ctrl *)data);
		break;
	case VIDIOC_QUERYMENU:
		error = vctrl_ioctl_querymenu(v->ctrl_handler, (struct v4l2_querymenu *)data);
		break;
	case VIDIOC_G_CTRL:
		error = vctrl_ioctl_g_ctrl(v->ctrl_handler, (struct v4l2_control *)data);
		break;
	case VIDIOC_S_CTRL:
		error = vctrl_ioctl_s_ctrl(v, (struct v4l2_control *)data);
		break;
	case VIDIOC_G_EXT_CTRLS:
	case VIDIOC_S_EXT_CTRLS:
	case VIDIOC_TRY_EXT_CTRLS:
		error = vctrl_ioctl_ext_ctrls(v, cmd, (struct v4l2_ext_controls *)data);
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

	if (v == NULL || v->queue == NULL)
		return (POLLERR);
	return (vbq_poll(v->queue, events, td));
}

static int
vdev_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct vdev *v = dev->si_drv1;

	if (v == NULL || v->queue == NULL)
		return (EINVAL);
	return (vbq_kqfilter(v->queue, kn));
}

static int
vdev_mmap_single(struct cdev *dev, vm_ooffset_t *offset, vm_size_t size,
    vm_object_t *object, int nprot)
{
	struct vdev *v = dev->si_drv1;

	if (v == NULL || v->queue == NULL)
		return (ENODEV);
	return (vbq_mmap_single(v->queue, offset, size, object, nprot));
}

/* -------------------------------------------------- register / unregister */

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

	mtx_lock(&vdev_mtx);
	while ((fh = TAILQ_FIRST(&v->fhs)) != NULL) {
		TAILQ_REMOVE(&v->fhs, fh, link);
		fh->vdev = NULL;
	}
	while (v->nclosing != 0)
		cv_wait(&vdev_cv, &vdev_mtx);
	v->registered = false;
	vdev_count--;
	mtx_unlock(&vdev_mtx);

	if (v->queue != NULL)
		vbq_shutdown(v->queue);
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
		break;
	case MOD_UNLOAD:
		if (vdev_count != 0) {
			error = EBUSY;
			break;
		}
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
