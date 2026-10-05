/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The V4L2 buffer queue, memory type MMAP.
 *
 * Buffer states and who owns a buffer (see enum vbuf_state in media.h):
 *
 *	DEQUEUED --QBUF--> QUEUED --start/QBUF--> ACTIVE --vbq_buf_done()--> DONE/ERROR
 *	    ^                                                                    |
 *	    +---------------------------------DQBUF------------------------------+
 *
 * `q->queued` holds every buffer from QBUF to DQBUF (QUEUED, ACTIVE, DONE and
 * ERROR), `q->done` the completed ones in completion order. All of it is
 * protected by q->mtx. Methods of the driver are called without q->mtx held,
 * so they may sleep and may call vbq_buf_done().
 *
 * Locking: v->lock (ioctl serialisation) is taken before q->mtx. DQBUF is the
 * one ioctl that does not hold v->lock: it sleeps, and STREAMOFF from another
 * thread must be able to wake it.
 *
 * Memory: each buffer owns its busdma tag, one contiguous segment allocated
 * with bus_dmamem_alloc(BUS_DMA_COHERENT), so the attribute userland gets
 * through mmap() is the one the kernel mapping has (read back from the page,
 * not guessed). mmap() goes through a device pager with the buffer as handle:
 * that gives one vm object per buffer, and ctor/dtor tell us exactly when a
 * buffer is mapped. Buffers are reference counted, so memory userland still has
 * mapped survives close(2) and REQBUFS.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <sys/refcount.h>
#include <sys/rwlock.h>
#include <sys/ucred.h>
#include <sys/selinfo.h>
#include <sys/event.h>
#include <sys/sx.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VBQ, "vbq", "V4L2 buffer queue");

#define	VBQ_TSTAMP_FLAGS \
	(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_EOF)

/* ------------------------------------------------------------ buffer memory */

static void
vbuf_load_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct vbuf *b = arg;

	if (error == 0 && nsegs == 1)
		b->paddr = segs[0].ds_addr;
}

static void
vbuf_free(struct vbuf *b)
{

	if (b->vaddr != NULL) {
		bus_dmamap_unload(b->dmat, b->dmamap);
		bus_dmamem_free(b->dmat, b->vaddr, b->dmamap);
	}
	if (b->dmat != NULL)
		bus_dma_tag_destroy(b->dmat);
	free(b, M_VBQ);
}

static void
vbuf_put(struct vbuf *b)
{

	if (refcount_release(&b->refs))
		vbuf_free(b);
}

static struct vbuf *
vbuf_alloc(struct vbq *q, uint32_t index, size_t size)
{
	struct vbuf *b;
	size_t alloc = round_page(size);
	vm_page_t m;

	b = malloc(sizeof(*b), M_VBQ, M_WAITOK | M_ZERO);
	refcount_init(&b->refs, 1);
	b->q = q;
	b->index = index;
	b->size = size;
	b->state = VBUF_DEQUEUED;
	if (bus_dma_tag_create(bus_get_dma_tag(q->vdev->dev),
	    PAGE_SIZE, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    alloc, 1, alloc, 0, NULL, NULL, &b->dmat) != 0)
		goto fail;
	if (bus_dmamem_alloc(b->dmat, &b->vaddr,
	    BUS_DMA_NOWAIT | BUS_DMA_ZERO | BUS_DMA_COHERENT, &b->dmamap) != 0) {
		b->vaddr = NULL;
		goto fail;
	}
	if (bus_dmamap_load(b->dmat, b->dmamap, b->vaddr, alloc, vbuf_load_cb,
	    b, BUS_DMA_NOWAIT) != 0 || b->paddr == 0)
		goto fail;
	m = PHYS_TO_VM_PAGE(b->paddr);
	b->memattr = (m != NULL) ? pmap_page_get_memattr(m) : VM_MEMATTR_DEFAULT;
	return (b);
fail:
	vbuf_free(b);
	return (NULL);
}

/* ------------------------------------------------------------ mmap (pager) */

static int
vbuf_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
	struct vbuf *b = handle;

	*color = 0;
	refcount_acquire(&b->refs);
	atomic_add_int(&b->nmaps, 1);
	return (0);
}

static void
vbuf_pager_dtor(void *handle)
{
	struct vbuf *b = handle;

	atomic_subtract_int(&b->nmaps, 1);
	vbuf_put(b);
}

static int
vbuf_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot,
    vm_page_t *mres)
{
	struct vbuf *b = object->handle;
	vm_memattr_t memattr = object->memattr;
	vm_paddr_t paddr;
	vm_page_t page;

	if (offset >= round_page(b->size))
		return (VM_PAGER_FAIL);
	paddr = b->paddr + offset;

	if (((*mres)->flags & PG_FICTITIOUS) != 0) {
		/* Reuse the fake page the fault path handed us. */
		page = *mres;
		vm_page_updatefake(page, paddr, memattr);
	} else {
		/* Replace the placeholder page with a fake page for our memory. */
		VM_OBJECT_WUNLOCK(object);
		page = vm_page_getfake(paddr, memattr);
		VM_OBJECT_WLOCK(object);
		vm_page_replace(page, object, (*mres)->pindex, *mres);
		*mres = page;
	}
	vm_page_valid(page);
	return (VM_PAGER_OK);
}

static struct cdev_pager_ops vbuf_pager_ops = {
	.cdev_pg_ctor = vbuf_pager_ctor,
	.cdev_pg_dtor = vbuf_pager_dtor,
	.cdev_pg_fault = vbuf_pager_fault,
};

int
vbq_mmap_single(struct vbq *q, vm_ooffset_t *offset, vm_size_t size,
    vm_object_t *objp, int nprot)
{
	struct vbuf *b = NULL;
	vm_object_t obj;
	uint32_t i;

	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		if (q->bufs[i]->offset == (off_t)*offset) {
			b = q->bufs[i];
			refcount_acquire(&b->refs);	/* keep it while we allocate */
			break;
		}
	}
	mtx_unlock(&q->mtx);
	if (b == NULL || size == 0 || size > round_page(b->size)) {
		if (b != NULL)
			vbuf_put(b);
		return (EINVAL);
	}
	obj = cdev_pager_allocate(b, OBJT_DEVICE, &vbuf_pager_ops,
	    round_page(b->size), nprot, 0, curthread->td_ucred);
	if (obj != NULL && b->memattr != VM_MEMATTR_DEFAULT)
		vm_object_set_memattr(obj, b->memattr);
	vbuf_put(b);
	if (obj == NULL)
		return (ENOMEM);
	*offset = 0;
	*objp = obj;
	return (0);
}

/* ------------------------------------------------------------ queue basics */

void
vbq_init(struct vbq *q, struct vdev *v, enum v4l2_buf_type type,
    kobj_class_t cls)
{

	kobj_init((kobj_t)q, cls);
	q->vdev = v;
	q->type = type;
	q->min_queued = 1;
	mtx_init(&q->mtx, "vbq", NULL, MTX_DEF);
	knlist_init_mtx(&q->rsel.si_note, &q->mtx);
	TAILQ_INIT(&q->queued);
	TAILQ_INIT(&q->done);
}

static void vbq_cancel(struct vbq *);
static void vbq_free_bufs(struct vbq *);

void
vbq_destroy(struct vbq *q)
{

	vbq_cancel(q);
	vbq_free_bufs(q);
	seldrain(&q->rsel);
	knlist_destroy(&q->rsel.si_note);
	mtx_destroy(&q->mtx);
	kobj_delete((kobj_t)q, NULL);
}

bool
vbq_is_streaming(struct vbq *q)
{
	bool r;

	mtx_lock(&q->mtx);
	r = q->streaming;
	mtx_unlock(&q->mtx);
	return (r);
}

bool
vbq_has_buffers(struct vbq *q)
{

	return (q->nbufs != 0);
}

bool
vbq_busy_for(struct vbq *q, struct vfh *fh)
{

	return (q->owner != NULL && q->owner != fh);
}

/* Is any buffer still mapped by userland? */
static bool
vbq_any_mapped(struct vbq *q)
{
	uint32_t i;

	for (i = 0; i < q->nbufs; i++)
		if (atomic_load_int(&q->bufs[i]->nmaps) != 0)
			return (true);
	return (false);
}

/* Let go of every buffer. Buffers userland still maps live on until unmapped. */
static void
vbq_free_bufs(struct vbq *q)
{
	struct vbuf *b;
	uint32_t i;

	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		b = q->bufs[i];
		q->bufs[i] = NULL;
		b->q = NULL;
		b->state = VBUF_DEQUEUED;
		mtx_unlock(&q->mtx);
		vbuf_put(b);
		mtx_lock(&q->mtx);
	}
	q->nbufs = 0;
	q->next_offset = 0;
	TAILQ_INIT(&q->queued);
	TAILQ_INIT(&q->done);
	q->nqueued = q->nactive = 0;
	mtx_unlock(&q->mtx);
}

/*
 * Take every buffer back from the driver and from the done list; all of them
 * belong to userland again, as after STREAMOFF.
 */
static void
vbq_cancel(struct vbq *q)
{
	bool was_started;
	uint32_t i;

	mtx_lock(&q->mtx);
	was_started = q->started;
	q->streaming = false;
	q->started = false;
	mtx_unlock(&q->mtx);

	if (was_started)
		VBQ_STOP_STREAMING(q);

	mtx_lock(&q->mtx);
	if (q->nactive != 0)
		printf("vbq: %u buffers still active after stop_streaming\n",
		    q->nactive);
	for (i = 0; i < q->nbufs; i++) {
		q->bufs[i]->state = VBUF_DEQUEUED;
		q->bufs[i]->bytesused = 0;
	}
	TAILQ_INIT(&q->queued);
	TAILQ_INIT(&q->done);
	q->nqueued = q->nactive = 0;
	q->sequence = 0;
	q->error = false;
	wakeup(&q->done);
	selwakeuppri(&q->rsel, PRIBIO);
	KNOTE_LOCKED(&q->rsel.si_note, 0);
	mtx_unlock(&q->mtx);
}

/* ------------------------------------------------------------------ REQBUFS */

/*
 * Allocate `want` more buffers of `size` bytes. The driver sees the new total
 * and may change it. Returns how many were allocated (0 on failure).
 */
static uint32_t
vbq_alloc_bufs(struct vbq *q, uint32_t total, size_t size)
{
	uint32_t i;

	for (i = q->nbufs; i < total; i++) {
		struct vbuf *b = vbuf_alloc(q, i, size);

		if (b == NULL)
			break;
		b->offset = q->next_offset;
		q->next_offset += round_page(size);
		mtx_lock(&q->mtx);
		q->bufs[i] = b;
		q->nbufs = i + 1;
		mtx_unlock(&q->mtx);
	}
	return (i);
}

int
vbq_reqbufs(struct vbq *q, struct vfh *fh, struct v4l2_requestbuffers *rb)
{
	uint32_t n;
	size_t size = 0;
	int error;

	if (rb->type != q->type || rb->memory != V4L2_MEMORY_MMAP)
		return (EINVAL);
	rb->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
	rb->flags = 0;
	memset(rb->reserved, 0, sizeof(rb->reserved));

	if (q->streaming)
		return (EBUSY);
	if (vbq_any_mapped(q))
		return (EBUSY);
	/* Any earlier allocation is dropped first, as in Linux. */
	if (q->nbufs != 0) {
		vbq_free_bufs(q);
		q->owner = NULL;
	}
	if (rb->count == 0)
		return (0);

	n = MIN(rb->count, VBQ_MAX_BUFFERS);
	error = VBQ_QUEUE_SETUP(q, &n, &size);
	if (error != 0)
		return (error);
	if (n == 0 || size == 0)
		return (EINVAL);
	n = MIN(n, VBQ_MAX_BUFFERS);

	if (vbq_alloc_bufs(q, n, size) == 0)
		return (ENOMEM);
	rb->count = q->nbufs;
	q->owner = fh;
	return (0);
}

/*
 * VIDIOC_CREATE_BUFS: add buffers to what is there. `req_size` is the
 * sizeimage the caller asked for (0: the driver's); a size below what the
 * driver needs is refused, a larger one is used.
 */
int
vbq_create_bufs(struct vbq *q, struct vfh *fh, struct v4l2_create_buffers *cb,
    size_t req_size)
{
	uint32_t n, first = q->nbufs;
	size_t size = 0;
	int error;

	cb->index = first;
	cb->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
	cb->flags = 0;
	/* Older userland sees max_num_buffers as reserved[0]: keep it zero. */
	cb->max_num_buffers = 0;
	memset(cb->reserved, 0, sizeof(cb->reserved));
	if (cb->memory != V4L2_MEMORY_MMAP)
		return (EINVAL);
	if (cb->count == 0)
		return (0);
	if (q->nbufs >= VBQ_MAX_BUFFERS)
		return (ENOBUFS);

	n = MIN(first + cb->count, VBQ_MAX_BUFFERS);
	error = VBQ_QUEUE_SETUP(q, &n, &size);
	if (error != 0)
		return (error);
	if (size == 0)
		return (EINVAL);
	if (req_size != 0) {
		if (req_size < size)
			return (EINVAL);
		size = req_size;
	}
	n = MIN(n, VBQ_MAX_BUFFERS);
	if (n <= first)
		return (EINVAL);
	if (vbq_alloc_bufs(q, n, size) == first)
		return (ENOMEM);
	cb->count = q->nbufs - first;
	if (q->owner == NULL)
		q->owner = fh;
	return (0);
}

/* ---------------------------------------------------------- struct v4l2_buffer */

static void
vbq_fill(struct vbq *q, struct vbuf *b, struct v4l2_buffer *out)
{
	uint32_t flags = (b->flags & V4L2_BUF_FLAG_KEYFRAME) | VBQ_TSTAMP_FLAGS;

	memset(out, 0, sizeof(*out));
	out->index = b->index;
	out->type = q->type;
	out->memory = V4L2_MEMORY_MMAP;
	out->bytesused = b->bytesused;
	out->field = V4L2_FIELD_NONE;
	out->timestamp = b->timestamp;
	out->sequence = b->sequence;
	out->m.offset = (uint32_t)b->offset;
	out->length = b->size;
	switch (b->state) {
	case VBUF_PREPARED:
		flags |= V4L2_BUF_FLAG_PREPARED;
		break;
	case VBUF_QUEUED:
	case VBUF_ACTIVE:
		flags |= V4L2_BUF_FLAG_QUEUED;
		break;
	case VBUF_ERROR:
		flags |= V4L2_BUF_FLAG_ERROR;
		/* FALLTHROUGH */
	case VBUF_DONE:
		flags |= V4L2_BUF_FLAG_DONE;
		break;
	case VBUF_DEQUEUED:
		break;
	}
	if (atomic_load_int(&b->nmaps) != 0)
		flags |= V4L2_BUF_FLAG_MAPPED;
	out->flags = flags;
}

/*
 * QUERYBUF ignores the memory field of its input (Linux reports it, it does
 * not read it); QBUF and PREPARE_BUF insist on it.
 */
static struct vbuf *
vbq_lookup(struct vbq *q, struct v4l2_buffer *b, bool check_memory)
{

	if (b->type != q->type || b->index >= q->nbufs ||
	    (check_memory && b->memory != V4L2_MEMORY_MMAP))
		return (NULL);
	return (q->bufs[b->index]);
}

int
vbq_querybuf(struct vbq *q, struct v4l2_buffer *buf)
{
	struct vbuf *b;

	mtx_lock(&q->mtx);
	b = vbq_lookup(q, buf, false);
	if (b == NULL) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	vbq_fill(q, b, buf);
	mtx_unlock(&q->mtx);
	return (0);
}

int
vbq_prepare_buf(struct vbq *q, struct v4l2_buffer *buf)
{
	struct vbuf *b;
	int error;

	mtx_lock(&q->mtx);
	b = vbq_lookup(q, buf, true);
	if (b == NULL || b->state != VBUF_DEQUEUED) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	mtx_unlock(&q->mtx);
	/* v->lock serialises QBUF/PREPARE_BUF, so b stays DEQUEUED meanwhile. */
	error = VBQ_BUF_PREPARE(q, b);
	if (error != 0)
		return (error);
	mtx_lock(&q->mtx);
	b->state = VBUF_PREPARED;
	vbq_fill(q, b, buf);
	mtx_unlock(&q->mtx);
	return (0);
}

/* ---------------------------------------------------------------- streaming */

/* Hand b (state ACTIVE already) to the driver. */
static void
vbq_to_driver(struct vbq *q, struct vbuf *b)
{

	bus_dmamap_sync(b->dmat, b->dmamap,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	VBQ_BUF_QUEUE(q, b);
}

/*
 * Give every queued buffer to the driver and call start_streaming(). On
 * failure the buffers go back to QUEUED, as they would not have been
 * started at all.
 */
static int
vbq_start(struct vbq *q)
{
	struct vbuf *b;
	struct vbuf *list[VBQ_MAX_BUFFERS];
	uint32_t n = 0, count, i;
	int error;

	mtx_lock(&q->mtx);
	TAILQ_FOREACH(b, &q->queued, qentry) {
		if (b->state != VBUF_QUEUED)
			continue;
		b->state = VBUF_ACTIVE;
		q->nactive++;
		list[n++] = b;
	}
	count = q->nqueued;
	q->started = true;
	mtx_unlock(&q->mtx);

	for (i = 0; i < n; i++)
		vbq_to_driver(q, list[i]);
	error = VBQ_START_STREAMING(q, count);
	if (error == 0)
		return (0);

	mtx_lock(&q->mtx);
	q->started = false;
	TAILQ_FOREACH(b, &q->queued, qentry) {
		if (b->state == VBUF_ACTIVE) {
			b->state = VBUF_QUEUED;
			q->nactive--;
		}
	}
	mtx_unlock(&q->mtx);
	return (error);
}

int
vbq_qbuf(struct vbq *q, struct v4l2_buffer *buf)
{
	struct vbuf *b;
	bool give = false, start = false;
	int error;

	mtx_lock(&q->mtx);
	b = vbq_lookup(q, buf, true);
	if (b == NULL || (b->state != VBUF_DEQUEUED && b->state != VBUF_PREPARED)) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	if (b->state == VBUF_DEQUEUED) {
		mtx_unlock(&q->mtx);
		error = VBQ_BUF_PREPARE(q, b);
		if (error != 0)
			return (error);
		mtx_lock(&q->mtx);
	}
	b->state = VBUF_QUEUED;
	b->bytesused = 0;
	TAILQ_INSERT_TAIL(&q->queued, b, qentry);
	q->nqueued++;
	if (q->started) {
		b->state = VBUF_ACTIVE;
		q->nactive++;
		give = true;
	} else if (q->streaming && q->nqueued >= q->min_queued) {
		start = true;
	}
	vbq_fill(q, b, buf);
	mtx_unlock(&q->mtx);

	if (give)
		vbq_to_driver(q, b);
	else if (start) {
		error = vbq_start(q);
		if (error != 0) {
			/* The buffer stays queued; STREAMON-style failure. */
			return (error);
		}
		mtx_lock(&q->mtx);
		vbq_fill(q, b, buf);
		mtx_unlock(&q->mtx);
	}
	return (0);
}

int
vbq_streamon(struct vbq *q, enum v4l2_buf_type type)
{
	int error = 0;

	if (type != q->type)
		return (EINVAL);
	mtx_lock(&q->mtx);
	if (q->nbufs == 0) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	if (q->streaming) {
		mtx_unlock(&q->mtx);
		return (0);
	}
	q->streaming = true;
	q->sequence = 0;
	if (q->nqueued >= q->min_queued) {
		mtx_unlock(&q->mtx);
		error = vbq_start(q);
		if (error != 0) {
			mtx_lock(&q->mtx);
			q->streaming = false;
			mtx_unlock(&q->mtx);
		}
		return (error);
	}
	mtx_unlock(&q->mtx);
	return (0);
}

int
vbq_streamoff(struct vbq *q, enum v4l2_buf_type type)
{

	if (type != q->type)
		return (EINVAL);
	vbq_cancel(q);
	return (0);
}

void
vbq_buf_done(struct vbuf *b, bool error)
{
	struct vbq *q = b->q;

	if (q == NULL)
		return;
	if (b->timestamp.tv_sec == 0 && b->timestamp.tv_usec == 0)
		microuptime(&b->timestamp);
	bus_dmamap_sync(b->dmat, b->dmamap, BUS_DMASYNC_POSTREAD);
	mtx_lock(&q->mtx);
	if (b->state != VBUF_ACTIVE) {
		mtx_unlock(&q->mtx);
		printf("vbq: buffer %u completed while not active\n", b->index);
		return;
	}
	b->sequence = q->sequence++;
	b->state = error ? VBUF_ERROR : VBUF_DONE;
	if (error)
		b->bytesused = 0;
	q->nactive--;
	TAILQ_INSERT_TAIL(&q->done, b, dentry);
	wakeup(&q->done);
	selwakeuppri(&q->rsel, PRIBIO);
	KNOTE_LOCKED(&q->rsel.si_note, 0);
	mtx_unlock(&q->mtx);
}

int
vbq_dqbuf(struct vbq *q, struct v4l2_buffer *buf, bool nonblock)
{
	struct vbuf *b;
	int error;

	if (buf->type != q->type)
		return (EINVAL);
	mtx_lock(&q->mtx);
	for (;;) {
		if (!q->streaming) {
			mtx_unlock(&q->mtx);
			return (EINVAL);
		}
		if (q->error) {
			mtx_unlock(&q->mtx);
			return (EIO);
		}
		if (!TAILQ_EMPTY(&q->done))
			break;
		if (nonblock) {
			mtx_unlock(&q->mtx);
			return (EAGAIN);
		}
		error = msleep(&q->done, &q->mtx, PCATCH | PRIBIO, "vbqdq", 0);
		if (error != 0) {
			mtx_unlock(&q->mtx);
			return (error);
		}
	}
	b = TAILQ_FIRST(&q->done);
	TAILQ_REMOVE(&q->done, b, dentry);
	TAILQ_REMOVE(&q->queued, b, qentry);
	q->nqueued--;
	vbq_fill(q, b, buf);		/* reports DONE/ERROR */
	b->state = VBUF_DEQUEUED;
	mtx_unlock(&q->mtx);
	return (0);
}

/* -------------------------------------------------------------- poll, kqueue */

int
vbq_poll(struct vbq *q, int events, struct thread *td)
{
	int revents = 0;

	if ((events & (POLLIN | POLLRDNORM)) == 0)
		return (0);
	mtx_lock(&q->mtx);
	if (!q->streaming || q->error || TAILQ_EMPTY(&q->queued))
		revents = POLLERR;
	else if (!TAILQ_EMPTY(&q->done))
		revents = events & (POLLIN | POLLRDNORM);
	else
		selrecord(td, &q->rsel);
	mtx_unlock(&q->mtx);
	return (revents);
}

static void
vbq_kn_detach(struct knote *kn)
{
	struct vbq *q = kn->kn_hook;

	knlist_remove(&q->rsel.si_note, kn, 0);
}

static int
vbq_kn_event(struct knote *kn, long hint)
{
	struct vbq *q = kn->kn_hook;

	return (!TAILQ_EMPTY(&q->done));
}

static const struct filterops vbq_read_filterops = {
	.f_isfd = 1,
	.f_detach = vbq_kn_detach,
	.f_event = vbq_kn_event,
};

int
vbq_kqfilter(struct vbq *q, struct knote *kn)
{

	if (kn->kn_filter != EVFILT_READ)
		return (EINVAL);
	kn->kn_fop = &vbq_read_filterops;
	kn->kn_hook = q;
	knlist_add(&q->rsel.si_note, kn, 0);
	return (0);
}

/* ---------------------------------------------------------------- release */

void
vbq_release(struct vbq *q, struct vfh *fh)
{

	if (q->owner != fh)
		return;
	vbq_cancel(q);
	vbq_free_bufs(q);
	q->owner = NULL;
}

void
vbq_shutdown(struct vbq *q)
{

	vbq_cancel(q);
	vbq_free_bufs(q);
	q->owner = NULL;
}
