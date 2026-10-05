/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The media request API.
 *
 * A request gathers the buffers and the control values that belong to one unit
 * of work (one frame to decode, say) so that a driver applies them together.
 * Userland gets one from /dev/mediaN (MEDIA_IOC_REQUEST_ALLOC), binds buffers
 * to it with QBUF and control values with S_EXT_CTRLS (which =
 * V4L2_CTRL_WHICH_REQUEST_VAL), and then queues it. Once every buffer of a
 * queued request is done the request is complete: poll(2) reports POLLPRI and
 * the values of the controls as they were applied can be read back. REINIT
 * makes it empty and reusable.
 *
 * The descriptor is a file of its own type. It stays valid when the media
 * device node is closed.
 *
 * References: the descriptor holds one, and so does every bound buffer, so a
 * queued request outlives its descriptor. The request holds one reference on
 * each buffer in its list for the same reason.
 *
 * State: IDLE -> QUEUED (QUEUE) -> COMPLETE (last buffer done or taken back)
 * -> IDLE (REINIT). Only IDLE and COMPLETE can be re-initialised or closed
 * with the buffers still bound; the others wait for the drivers.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/capsicum.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/selinfo.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/user.h>

#include <dev/media/media_priv.h>

#ifndef DTYPE_VREQ
#define	DTYPE_VREQ	101	/* local; told apart by the fileops, not this */
#endif

MALLOC_DEFINE(M_VREQ, "vreq", "media requests");

static struct sx vreq_sx;
static struct mtx vreq_list_mtx;
static TAILQ_HEAD(, vreq) vreq_all = TAILQ_HEAD_INITIALIZER(vreq_all);
static u_int vreq_count;

static fo_ioctl_t	vreq_ioctl_f;
static fo_poll_t	vreq_poll_f;
static fo_stat_t	vreq_stat_f;
static fo_seek_t	vreq_seek_f;
static fo_close_t	vreq_close_f;
static fo_fill_kinfo_t	vreq_fill_kinfo_f;

static const struct fileops vreq_fileops = {
	.fo_read = invfo_rdwr,
	.fo_write = invfo_rdwr,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = vreq_ioctl_f,
	.fo_poll = vreq_poll_f,
	.fo_kqfilter = invfo_kqfilter,
	.fo_stat = vreq_stat_f,
	.fo_close = vreq_close_f,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_seek = vreq_seek_f,
	.fo_fill_kinfo = vreq_fill_kinfo_f,
	.fo_cmp = file_kcmp_generic,
	.fo_flags = DFLAG_PASSABLE,
};

void
vreq_init(void)
{

	sx_init(&vreq_sx, "vreq");
	mtx_init(&vreq_list_mtx, "vreq list", NULL, MTX_DEF);
}

int
vreq_fini(void)
{

	if (atomic_load_int(&vreq_count) != 0)
		return (EBUSY);
	mtx_destroy(&vreq_list_mtx);
	sx_destroy(&vreq_sx);
	return (0);
}

u_int
vreq_live(void)
{

	return (atomic_load_int(&vreq_count));
}

void vreq_lock(void) { sx_xlock(&vreq_sx); }
void vreq_unlock(void) { sx_xunlock(&vreq_sx); }
void vreq_slock(void) { sx_slock(&vreq_sx); }
void vreq_sunlock(void) { sx_sunlock(&vreq_sx); }

/* ------------------------------------------------------------ the object */

static void
vreq_free_ctrls(struct vreq *req)
{
	struct vreq_ctrl *e;

	while ((e = TAILQ_FIRST(&req->ctrls)) != NULL) {
		TAILQ_REMOVE(&req->ctrls, e, link);
		free(e->data, M_VREQ);
		free(e, M_VREQ);
	}
}

void
vreq_put(struct vreq *req)
{

	if (!refcount_release(&req->refs))
		return;
	mtx_lock(&vreq_list_mtx);
	TAILQ_REMOVE(&vreq_all, req, gentry);
	mtx_unlock(&vreq_list_mtx);
	vreq_free_ctrls(req);
	seldrain(&req->sel);
	knlist_destroy(&req->sel.si_note);
	sx_destroy(&req->clk);
	mtx_destroy(&req->mtx);
	free(req, M_VREQ);
	atomic_subtract_int(&vreq_count, 1);
}

int
vreq_alloc(struct vmedia *m, struct thread *td, int *fdp)
{
	struct vreq *req;
	struct file *fp;
	int error, fd;

	req = malloc(sizeof(*req), M_VREQ, M_WAITOK | M_ZERO);
	refcount_init(&req->refs, 1);
	req->media_serial = m->serial_no;
	req->state = VREQ_IDLE;
	mtx_init(&req->mtx, "vreq", NULL, MTX_DEF);
	sx_init(&req->clk, "vreq ctrl");
	TAILQ_INIT(&req->bufs);
	TAILQ_INIT(&req->ctrls);
	knlist_init_mtx(&req->sel.si_note, &req->mtx);

	error = falloc(td, &fp, &fd, O_CLOEXEC);
	if (error != 0) {
		knlist_destroy(&req->sel.si_note);
		sx_destroy(&req->clk);
		mtx_destroy(&req->mtx);
		free(req, M_VREQ);
		return (error);
	}
	atomic_add_int(&vreq_count, 1);
	mtx_lock(&vreq_list_mtx);
	TAILQ_INSERT_TAIL(&vreq_all, req, gentry);
	mtx_unlock(&vreq_list_mtx);
	finit(fp, FREAD | FWRITE, DTYPE_VREQ, req, &vreq_fileops);
	*fdp = fd;
	fdrop(fp, td);
	return (0);
}

int
vreq_from_fd(int fd, uint64_t serial, struct thread *td, struct vreq **reqp)
{
	struct file *fp;
	struct vreq *req;

	if (fget(td, fd, &cap_no_rights, &fp) != 0)
		return (EINVAL);
	if (fp->f_ops != &vreq_fileops) {
		fdrop(fp, td);
		return (EINVAL);
	}
	req = fp->f_data;
	if (req->media_serial != serial) {
		fdrop(fp, td);
		return (EINVAL);
	}
	refcount_acquire(&req->refs);
	fdrop(fp, td);
	*reqp = req;
	return (0);
}

bool
vreq_is_state(struct vreq *req, enum vreq_state st)
{
	bool r;

	mtx_lock(&req->mtx);
	r = (req->state == st);
	mtx_unlock(&req->mtx);
	return (r);
}

/* ----------------------------------------------------------- bound buffers */

int
vreq_bind(struct vreq *req, struct vbuf *b)
{

	sx_assert(&vreq_sx, SA_XLOCKED);
	if (req->state != VREQ_IDLE)
		return (EBUSY);
	refcount_acquire(&req->refs);		/* the buffer's */
	vbuf_hold(b);				/* the request's */
	b->req = req;
	b->reqlist = req;
	b->req_done = false;
	TAILQ_INSERT_TAIL(&req->bufs, b, reqentry);
	mtx_lock(&req->mtx);
	req->pending++;
	mtx_unlock(&req->mtx);
	return (0);
}

void
vreq_buf_finished(struct vreq *req, struct vbuf *b)
{

	mtx_lock(&req->mtx);
	if (!b->req_done) {
		b->req_done = true;
		req->pending--;
		if (req->pending == 0 && req->state == VREQ_QUEUED) {
			req->state = VREQ_COMPLETE;
			selwakeuppri(&req->sel, PRIBIO);
			KNOTE_LOCKED(&req->sel.si_note, 0);
		}
	}
	mtx_unlock(&req->mtx);
}

void
vreq_unbind(struct vbuf *b)
{
	struct vreq *req = b->reqlist;

	sx_assert(&vreq_sx, SA_XLOCKED);
	if (req == NULL)
		return;
	vreq_buf_finished(req, b);
	TAILQ_REMOVE(&req->bufs, b, reqentry);
	b->reqlist = NULL;
	vbuf_put(b);
	vreq_put(req);
}

/*
 * Take every buffer out of the request and forget its control values. The
 * buffers go back to userland: one that was only waiting for its request is
 * DEQUEUED again, one that finished stays as it is.
 */
static void
vreq_clear(struct vreq *req)
{
	struct vbuf *b, *tmp;
	struct vbq *q;

	sx_assert(&vreq_sx, SA_XLOCKED);
	/* Each unbind may drop the last reference on b, so keep the request alive. */
	refcount_acquire(&req->refs);
	TAILQ_FOREACH_SAFE(b, &req->bufs, reqentry, tmp) {
		q = b->q;
		if (q != NULL) {
			mtx_lock(&q->mtx);
			if (b->req == req) {
				b->req = NULL;
				b->request_fd = 0;
				if (b->state == VBUF_IN_REQUEST)
					b->state = VBUF_DEQUEUED;
			}
			mtx_unlock(&q->mtx);
		}
		vreq_unbind(b);
	}
	sx_xlock(&req->clk);
	vreq_free_ctrls(req);
	req->hdl = NULL;
	sx_xunlock(&req->clk);
	mtx_lock(&req->mtx);
	req->pending = 0;
	req->state = VREQ_IDLE;
	mtx_unlock(&req->mtx);
	vreq_put(req);
}

/* -------------------------------------------------------- control values */

struct vreq_ctrl *
vreq_ctrl_find(struct vreq *req, uint32_t id)
{
	struct vreq_ctrl *e;

	sx_assert(&req->clk, SA_LOCKED);
	TAILQ_FOREACH(e, &req->ctrls, link)
		if (e->id == id)
			return (e);
	return (NULL);
}

struct vreq_ctrl *
vreq_ctrl_add(struct vreq *req, uint32_t id)
{
	struct vreq_ctrl *e;

	sx_assert(&req->clk, SA_XLOCKED);
	e = malloc(sizeof(*e), M_VREQ, M_WAITOK | M_ZERO);
	e->id = id;
	TAILQ_INSERT_TAIL(&req->ctrls, e, link);
	return (e);
}

void
vreq_handler_gone(struct vctrl_handler *h)
{
	struct vreq *req, *list[64];
	u_int i, n;

	/* Collect, with references, then work without the list lock. */
	for (;;) {
		n = 0;
		mtx_lock(&vreq_list_mtx);
		TAILQ_FOREACH(req, &vreq_all, gentry) {
			if (req->hdl != h)
				continue;
			refcount_acquire(&req->refs);
			list[n++] = req;
			if (n == nitems(list))
				break;
		}
		mtx_unlock(&vreq_list_mtx);
		for (i = 0; i < n; i++) {
			sx_xlock(&list[i]->clk);
			if (list[i]->hdl == h)
				list[i]->hdl = NULL;
			sx_xunlock(&list[i]->clk);
			vreq_put(list[i]);
		}
		if (n < nitems(list))
			break;
	}
}

/* ------------------------------------------------------------ the ioctls */

static int
vreq_do_queue(struct vreq *req)
{
	struct vbuf *b;

	vreq_lock();
	if (req->state != VREQ_IDLE) {
		vreq_unlock();
		return (EBUSY);
	}
	if (TAILQ_EMPTY(&req->bufs)) {
		vreq_unlock();
		return (ENOENT);
	}
	mtx_lock(&req->mtx);
	req->state = VREQ_QUEUED;
	mtx_unlock(&req->mtx);
	TAILQ_FOREACH(b, &req->bufs, reqentry)
		vbq_request_queue_buf(b->q, b);
	vreq_unlock();
	return (0);
}

static int
vreq_do_reinit(struct vreq *req)
{

	vreq_lock();
	if (req->state == VREQ_QUEUED) {
		vreq_unlock();
		return (EBUSY);
	}
	vreq_clear(req);
	vreq_unlock();
	return (0);
}

static int
vreq_ioctl_f(struct file *fp, u_long cmd, void *data, struct ucred *cred,
    struct thread *td)
{
	struct vreq *req = fp->f_data;

	switch (cmd) {
	case MEDIA_REQUEST_IOC_QUEUE:
		return (vreq_do_queue(req));
	case MEDIA_REQUEST_IOC_REINIT:
		return (vreq_do_reinit(req));
	default:
		return (ENOTTY);
	}
}

static int
vreq_poll_f(struct file *fp, int events, struct ucred *cred, struct thread *td)
{
	struct vreq *req = fp->f_data;
	int revents = 0;

	if ((events & (POLLPRI | POLLRDBAND)) == 0)
		return (0);
	mtx_lock(&req->mtx);
	if (req->state == VREQ_COMPLETE)
		revents = events & (POLLPRI | POLLRDBAND);
	else
		selrecord(td, &req->sel);
	mtx_unlock(&req->mtx);
	return (revents);
}

static int
vreq_stat_f(struct file *fp, struct stat *sb, struct ucred *active_cred)
{

	bzero(sb, sizeof(*sb));
	sb->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
	return (0);
}

static int
vreq_seek_f(struct file *fp, off_t offset, int whence, struct thread *td)
{

	return (ESPIPE);
}

static int
vreq_fill_kinfo_f(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{

	kif->kf_type = KF_TYPE_UNKNOWN;
	return (0);
}

/*
 * The last descriptor went away. An idle or complete request lets go of its
 * buffers; a queued one lives on until the drivers are done with them.
 */
static int
vreq_close_f(struct file *fp, struct thread *td)
{
	struct vreq *req = fp->f_data;

	vreq_lock();
	if (req->state != VREQ_QUEUED)
		vreq_clear(req);
	vreq_unlock();
	vreq_put(req);
	return (0);
}
