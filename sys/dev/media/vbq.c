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
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>

#include <sys/fcntl.h>
#include <sys/sglist.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VBQ, "vbq", "V4L2 buffer queue");


/*
 * Buffers that exist, including ones only a mmap() keeps alive. Their vm
 * objects call into this module (the device pager), so the module cannot be
 * unloaded while any is left.
 */
static u_int vbuf_live;

u_int
vbq_live_buffers(void)
{

	return (atomic_load_int(&vbuf_live));
}

#define	VBQ_TSTAMP_FLAGS(q) \
	((q)->ts_copy ? V4L2_BUF_FLAG_TIMESTAMP_COPY : \
	    (V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_EOF))

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

	if (b->dbuf != NULL)
		dmabuf_put(b->dbuf);
	if (b->imported) {
		/* nothing of ours: the memory belongs to the exporter */
	} else if (b->dmat == NULL) {
		if (b->vaddr != NULL)
			kmem_free(b->vaddr, round_page(b->size));	/* pages mode */
	} else {
		if (b->vaddr != NULL) {
			bus_dmamap_unload(b->dmat, b->dmamap);
			bus_dmamem_free(b->dmat, b->vaddr, b->dmamap);
		}
		bus_dma_tag_destroy(b->dmat);
	}
	free(b, M_VBQ);
	atomic_subtract_int(&vbuf_live, 1);
}

void
vbuf_put(struct vbuf *b)
{

	if (refcount_release(&b->refs))
		vbuf_free(b);
}

void
vbuf_hold(struct vbuf *b)
{

	refcount_acquire(&b->refs);
}

static struct vbuf *
vbuf_alloc(struct vbq *q, uint32_t index, size_t size, bool empty)
{
	struct vbuf *b;
	size_t alloc = round_page(size);
	vm_page_t m;

	b = malloc(sizeof(*b), M_VBQ, M_WAITOK | M_ZERO);
	atomic_add_int(&vbuf_live, 1);
	refcount_init(&b->refs, 1);
	b->q = q;
	b->index = index;
	b->size = size;
	b->state = VBUF_DEQUEUED;
	b->offset = -1;			/* no mmap offset until it has memory of its own */
	if (empty) {
		/* import mode: the memory comes with each QBUF */
		b->imported = true;
		return (b);
	}
	if (q->pages) {
		/*
		 * Software devices: separate pages, contiguous only in kernel
		 * virtual memory. They cannot fail for want of a physically
		 * contiguous run, and userland maps them page by page.
		 */
		b->vaddr = (void *)kmem_alloc_attr(alloc, M_NOWAIT | M_ZERO, 0,
		    ~(vm_paddr_t)0, VM_MEMATTR_DEFAULT);
		if (b->vaddr == NULL)
			goto fail;
		b->memattr = VM_MEMATTR_DEFAULT;
		return (b);
	}
	if (bus_dma_tag_create(bus_get_dma_tag(q->vdev->dev),
	    PAGE_SIZE, 0, q->dma_maxaddr != 0 ? q->dma_maxaddr : BUS_SPACE_MAXADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL,
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
	paddr = b->dmat != NULL ? b->paddr + offset :
	    vtophys((vm_offset_t)b->vaddr + offset);

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

/* A user-mappable vm object over the whole of buffer b (one per buffer, shared). */
static int
vbuf_mmap_object(struct vbuf *b, vm_size_t size, int nprot, vm_object_t *objp)
{
	vm_object_t obj;

	if (b->imported || size == 0 || size > round_page(b->size))
		return (EINVAL);
	obj = cdev_pager_allocate(b, OBJT_DEVICE, &vbuf_pager_ops,
	    round_page(b->size), nprot, 0, curthread->td_ucred);
	if (obj != NULL && b->memattr != VM_MEMATTR_DEFAULT)
		vm_object_set_memattr(obj, b->memattr);
	if (obj == NULL)
		return (ENOMEM);
	*objp = obj;
	return (0);
}

int
vbq_mmap_single(struct vbq *q, vm_ooffset_t *offset, vm_size_t size,
    vm_object_t *objp, int nprot)
{
	struct vbuf *b = NULL;
	uint32_t i;
	int error;

	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		if (q->bufs[i]->offset == (off_t)*offset) {
			b = q->bufs[i];
			refcount_acquire(&b->refs);	/* keep it while we allocate */
			break;
		}
	}
	mtx_unlock(&q->mtx);
	if (b == NULL)
		return (EINVAL);
	error = vbuf_mmap_object(b, size, nprot, objp);
	vbuf_put(b);
	if (error == 0)
		*offset = 0;
	return (error);
}

/* ----------------------------------------------------------- export (dmabuf) */

static int
vbuf_dmabuf_mmap(struct dmabuf *db, vm_ooffset_t *offset, vm_size_t size,
    vm_object_t *objp, int nprot)
{

	/* The object covers the whole buffer: the file offset is the offset in it. */
	return (vbuf_mmap_object(db->priv, round_page(db->size), nprot, objp));
}

static void *
vbuf_dmabuf_vmap(struct dmabuf *db)
{

	return (((struct vbuf *)db->priv)->vaddr);
}

static int
vbuf_dmabuf_sglist(struct dmabuf *db, struct sglist **sgp)
{
	struct vbuf *b = db->priv;
	struct sglist *sg;
	size_t off, alloc = round_page(b->size);
	int error = 0;

	sg = sglist_alloc(b->dmat != NULL ? 1 : alloc / PAGE_SIZE, M_WAITOK);
	if (b->dmat != NULL) {
		error = sglist_append_phys(sg, b->paddr, alloc);
	} else {
		for (off = 0; off < alloc && error == 0; off += PAGE_SIZE)
			error = sglist_append_phys(sg,
			    vtophys((vm_offset_t)b->vaddr + off), PAGE_SIZE);
	}
	if (error != 0) {
		sglist_free(sg);
		return (error);
	}
	*sgp = sg;
	return (0);
}

static void
vbuf_dmabuf_release(struct dmabuf *db)
{

	vbuf_put(db->priv);		/* the reference the export took */
}

static const struct dmabuf_ops vbuf_dmabuf_ops = {
	.mmap = vbuf_dmabuf_mmap,
	.vmap = vbuf_dmabuf_vmap,
	.sglist = vbuf_dmabuf_sglist,
	.release = vbuf_dmabuf_release,
};

/*
 * VIDIOC_EXPBUF: a file descriptor for the memory of a buffer. The dmabuf holds
 * a reference on the buffer, so the descriptor stays good after the queue let
 * the buffer go.
 */
int
vbq_expbuf(struct vbq *q, struct v4l2_exportbuffer *exp)
{
	struct vbuf *b;
	struct dmabuf *db;
	int error, fd;

	memset(exp->reserved, 0, sizeof(exp->reserved));
	if (exp->type != q->type || exp->plane != 0 ||
	    (exp->flags & ~(O_CLOEXEC | O_ACCMODE)) != 0)
		return (EINVAL);
	mtx_lock(&q->mtx);
	if (q->memory != V4L2_MEMORY_MMAP || exp->index >= q->nbufs) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	b = q->bufs[exp->index];
	refcount_acquire(&b->refs);
	mtx_unlock(&q->mtx);

	db = dmabuf_alloc(&vbuf_dmabuf_ops, round_page(b->size), b);	/* owns the reference */
	error = dmabuf_to_fd(db, exp->flags, curthread, &fd);
	dmabuf_put(db);			/* ours; the descriptor has its own */
	if (error != 0)
		return (error);
	exp->fd = fd;
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
	q->is_output = (type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
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

/*
 * The buffer whose timestamp is `ns` (seconds * 10^9 + microseconds * 10^3), or
 * NULL: how a stateless decoder finds the reference pictures a request names.
 * Only buffers that have been through a request carry a timestamp of interest;
 * the caller must not let the queue go (REQBUFS) while it uses the result.
 */
struct vbuf *
vbq_find_timestamp(struct vbq *q, uint64_t ns)
{
	struct vbuf *b, *found = NULL;
	uint32_t i;

	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		b = q->bufs[i];
		if (b != NULL && (uint64_t)b->timestamp.tv_sec * 1000000000ULL +
		    (uint64_t)b->timestamp.tv_usec * 1000ULL == ns) {
			found = b;
			break;
		}
	}
	mtx_unlock(&q->mtx);
	return (found);
}

bool
vbq_busy_for(struct vbq *q, struct vfh *fh)
{

	return (q->owner != NULL && q->owner != fh);
}

/* Let go of every buffer. Buffers userland still maps live on until unmapped. */
static void
vbq_free_bufs(struct vbq *q)
{
	struct vbuf *b;
	uint32_t i;
	bool reqs = q->uses_requests;

	/* Buffers bound to requests leave them first; see vreq_lock(). */
	if (reqs)
		vreq_lock();
	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		b = q->bufs[i];
		q->bufs[i] = NULL;
		b->q = NULL;
		b->state = VBUF_DEQUEUED;
		b->req = NULL;
		b->request_fd = 0;
		b->prepared = false;
		mtx_unlock(&q->mtx);
		if (reqs)
			vreq_unbind(b);
		vbuf_put(b);
		mtx_lock(&q->mtx);
	}
	q->uses_requests = q->uses_qbuf = false;
	q->nbufs = 0;
	q->memory = 0;
	q->next_offset = 0;
	TAILQ_INIT(&q->queued);
	TAILQ_INIT(&q->done);
	q->nqueued = q->nactive = 0;
	mtx_unlock(&q->mtx);
	if (reqs)
		vreq_unlock();
}

/*
 * Take every buffer back from the driver and from the done list; all of them
 * belong to userland again, as after STREAMOFF.
 */
static void
vbq_cancel(struct vbq *q)
{
	bool was_started, reqs = q->uses_requests;
	uint32_t i;

	/*
	 * Requests with buffers on this queue are taken apart too, and nothing
	 * may queue one meanwhile: see vreq_lock().
	 */
	if (reqs)
		vreq_lock();
	mtx_lock(&q->mtx);
	was_started = q->started;
	q->streaming = false;
	q->started = false;
	mtx_unlock(&q->mtx);

	if (was_started)
		VBQ_STOP_STREAMING(q);

	/* stop_streaming() has stopped the hardware: every buffer is ours again. */
	mtx_lock(&q->mtx);
	for (i = 0; i < q->nbufs; i++) {
		q->bufs[i]->state = VBUF_DEQUEUED;
		q->bufs[i]->req = NULL;
		q->bufs[i]->request_fd = 0;
		q->bufs[i]->prepared = false;
		q->bufs[i]->bytesused = 0;
		q->bufs[i]->timestamp.tv_sec = q->bufs[i]->timestamp.tv_usec = 0;
		q->bufs[i]->sequence = 0;
	}
	TAILQ_INIT(&q->queued);
	TAILQ_INIT(&q->done);
	q->nqueued = q->nactive = 0;
	q->sequence = 0;
	q->error = false;
	q->uses_requests = q->uses_qbuf = false;
	wakeup(&q->done);
	selwakeuppri(&q->vdev->sel, PRIBIO);
	KNOTE_LOCKED(&q->rsel.si_note, 0);
	mtx_unlock(&q->mtx);
	if (reqs) {
		for (i = 0; i < q->nbufs; i++)
			vreq_unbind(q->bufs[i]);
		vreq_unlock();
	}
}

/* ------------------------------------------------------------------ REQBUFS */

/*
 * Buffers may be freed while userland still maps them or holds an exported
 * descriptor: they live on until the last user lets go (see struct vbuf), which
 * is what V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS promises.
 */
#define	VBQ_CAPS(q) \
	(V4L2_BUF_CAP_SUPPORTS_MMAP | V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS | \
	    ((q)->dmabuf ? V4L2_BUF_CAP_SUPPORTS_DMABUF : 0) | \
	    ((q)->supports_requests && (q)->vdev->mdev != NULL ? \
	    V4L2_BUF_CAP_SUPPORTS_REQUESTS : 0))

/* Memory types this queue can have: MMAP always, DMABUF if the driver allows import. */
static bool
vbq_memory_ok(struct vbq *q, uint32_t memory)
{

	return (memory == V4L2_MEMORY_MMAP ||
	    (memory == V4L2_MEMORY_DMABUF && q->dmabuf));
}

/*
 * Allocate `want` more buffers of `size` bytes. The driver sees the new total
 * and may change it. Returns how many were allocated (0 on failure).
 */
static uint32_t
vbq_alloc_bufs(struct vbq *q, uint32_t total, size_t size, uint32_t memory)
{
	uint32_t i;

	for (i = q->nbufs; i < total; i++) {
		struct vbuf *b = vbuf_alloc(q, i, size,
		    memory == V4L2_MEMORY_DMABUF);

		if (b == NULL)
			break;
		if (memory == V4L2_MEMORY_MMAP) {
			b->offset = q->offset_base + q->next_offset;
			q->next_offset += round_page(size);
		}
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

	rb->capabilities = VBQ_CAPS(q);
	if (rb->type != q->type || !vbq_memory_ok(q, rb->memory))
		return (EINVAL);
	rb->flags = 0;
	memset(rb->reserved, 0, sizeof(rb->reserved));

	if (q->streaming)
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

	if (vbq_alloc_bufs(q, n, size, rb->memory) == 0)
		return (ENOMEM);
	rb->count = q->nbufs;
	q->memory = rb->memory;
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
	cb->capabilities = VBQ_CAPS(q);
	cb->flags = 0;
	/* Older userland sees max_num_buffers as reserved[0]: keep it zero. */
	cb->max_num_buffers = 0;
	memset(cb->reserved, 0, sizeof(cb->reserved));
	if (!vbq_memory_ok(q, cb->memory) ||
	    (q->nbufs != 0 && cb->memory != q->memory))
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
	if (vbq_alloc_bufs(q, n, size, cb->memory) == first)
		return (ENOMEM);
	q->memory = cb->memory;
	cb->count = q->nbufs - first;
	if (q->owner == NULL)
		q->owner = fh;
	return (0);
}

/* ---------------------------------------------------------- struct v4l2_buffer */

static void
vbq_fill(struct vbq *q, struct vbuf *b, struct v4l2_buffer *out)
{
	uint32_t flags = (b->flags & VBQ_FRAME_TYPE_FLAGS) | VBQ_TSTAMP_FLAGS(q);

	memset(out, 0, sizeof(*out));
	out->index = b->index;
	out->type = q->type;
	out->memory = q->memory != 0 ? q->memory : V4L2_MEMORY_MMAP;
	out->bytesused = b->bytesused;
	out->field = V4L2_FIELD_NONE;
	out->timestamp = b->timestamp;
	out->sequence = b->sequence;
	if (q->memory == V4L2_MEMORY_DMABUF)
		out->m.fd = b->dfd;
	else
		out->m.offset = (uint32_t)b->offset;
	out->length = b->size;
	if (b->req != NULL) {
		flags |= V4L2_BUF_FLAG_REQUEST_FD;
		out->request_fd = b->request_fd;
	}
	switch (b->state) {
	case VBUF_IN_REQUEST:
		flags |= V4L2_BUF_FLAG_IN_REQUEST;
		if (b->prepared)
			flags |= V4L2_BUF_FLAG_PREPARED;
		break;
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
		if (b->prepared)
			flags |= V4L2_BUF_FLAG_PREPARED;
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
	    (check_memory && b->memory != q->memory))
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

/*
 * DMABUF import: turn the descriptor userland queued into the buffer's memory.
 * The driver only ever uses b->vaddr; the buffer keeps a reference on the
 * dmabuf until it is queued with another one, or freed.
 */
static int
vbq_import(struct vbq *q, struct vbuf *b, const struct v4l2_buffer *buf)
{
	struct dmabuf *db;
	void *va;

	if (dmabuf_from_fd(buf->m.fd, curthread, &db) != 0)
		return (EINVAL);
	/* The caller's idea of the length, if given, and the buffer itself, must cover what we need. */
	if ((buf->length != 0 && buf->length < b->size) || db->size < b->size) {
		dmabuf_put(db);
		return (EINVAL);
	}
	va = dmabuf_vmap(db);
	if (va == NULL) {
		dmabuf_put(db);
		return (EINVAL);
	}
	if (b->dbuf != NULL)
		dmabuf_put(b->dbuf);
	b->dbuf = db;
	b->dfd = buf->m.fd;
	b->vaddr = va;
	return (0);
}

/*
 * What userland says about an OUTPUT buffer it filled: how much of it is data
 * (0 means all of it), and the frame type. Taken once, when the buffer is
 * prepared or queued from the dequeued state.
 */
static void
vbq_take_payload(struct vbq *q, struct vbuf *b, const struct v4l2_buffer *buf)
{

	if (!q->is_output)
		return;
	b->bytesused = buf->bytesused != 0 ? buf->bytesused : b->size;
	b->flags = buf->flags & VBQ_FRAME_TYPE_FLAGS;
}

int
vbq_prepare_buf(struct vbq *q, struct v4l2_buffer *buf)
{
	struct vbuf *b;
	int error;

	if ((buf->flags & V4L2_BUF_FLAG_REQUEST_FD) != 0)
		return (EINVAL);		/* PREPARE_BUF and requests do not mix */
	mtx_lock(&q->mtx);
	b = vbq_lookup(q, buf, true);
	if (b == NULL || b->state != VBUF_DEQUEUED || b->prepared ||
	    (q->is_output && buf->bytesused > b->size)) {
		mtx_unlock(&q->mtx);
		return (EINVAL);
	}
	mtx_unlock(&q->mtx);
	/* v->lock serialises QBUF/PREPARE_BUF, so b stays DEQUEUED meanwhile. */
	if (q->memory == V4L2_MEMORY_DMABUF) {
		error = vbq_import(q, b, buf);
		if (error != 0)
			return (error);
	}
	error = VBQ_BUF_PREPARE(q, b);
	if (error != 0)
		return (error);
	mtx_lock(&q->mtx);
	vbq_take_payload(q, b, buf);
	b->state = VBUF_PREPARED;
	b->prepared = true;
	vbq_fill(q, b, buf);
	mtx_unlock(&q->mtx);
	return (0);
}

/* ---------------------------------------------------------------- streaming */

/* Hand b (state ACTIVE already) to the driver. */
static void
vbq_to_driver(struct vbq *q, struct vbuf *b)
{

	if (b->dmat != NULL)
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

/*
 * Put b (DEQUEUED, PREPARED or IN_REQUEST with its request just queued) on the
 * queue's list and give it to the driver if the queue is running, or start the
 * queue if this buffer is the one it waited for. q->mtx is held on entry and
 * released.
 */
static int
vbq_enqueue_locked(struct vbq *q, struct vbuf *b)
{
	bool give = false, start = false;

	mtx_assert(&q->mtx, MA_OWNED);
	b->state = VBUF_QUEUED;
	b->prepared = false;
	b->sequence = 0;
	TAILQ_INSERT_TAIL(&q->queued, b, qentry);
	q->nqueued++;
	if (q->started) {
		b->state = VBUF_ACTIVE;
		q->nactive++;
		give = true;
	} else if (q->streaming && q->nqueued >= q->min_queued) {
		start = true;
	}
	mtx_unlock(&q->mtx);

	if (give) {
		vbq_to_driver(q, b);
		return (0);
	}
	return (start ? vbq_start(q) : 0);
}

int
vbq_qbuf(struct vbq *q, struct v4l2_buffer *buf)
{
	struct vbuf *b;
	struct vreq *req = NULL;
	bool use_req, was_prepared;
	int error;

	use_req = (buf->flags & V4L2_BUF_FLAG_REQUEST_FD) != 0;
	if (use_req) {
		if (!q->supports_requests || q->vdev->mdev == NULL)
			return (EBADR);
		error = vreq_from_fd(buf->request_fd, q->vdev->mdev->serial_no,
		    curthread, &req);
		if (error != 0)
			return (error);
		vreq_lock();
	} else if (q->requires_requests) {
		return (EBADR);
	}

	mtx_lock(&q->mtx);
	/* A queue is driven by requests or by QBUF, not both at once. */
	if (use_req ? q->uses_qbuf : q->uses_requests) {
		error = EBUSY;
		goto unlock_out;
	}
	b = vbq_lookup(q, buf, true);
	if (b == NULL || (b->state != VBUF_DEQUEUED && b->state != VBUF_PREPARED)) {
		error = EINVAL;
		goto unlock_out;
	}
	was_prepared = (b->state == VBUF_PREPARED);
	if (q->is_output && !was_prepared && buf->bytesused > b->size) {
		error = EINVAL;
		goto unlock_out;
	}
	if (!was_prepared && !b->prepared) {
		mtx_unlock(&q->mtx);
		if (q->memory == V4L2_MEMORY_DMABUF) {
			error = vbq_import(q, b, buf);
			if (error != 0)
				goto out;
		}
		error = VBQ_BUF_PREPARE(q, b);
		if (error != 0)
			goto out;
		mtx_lock(&q->mtx);
	}
	if (q->is_output) {
		/*
		 * The payload was taken at PREPARE_BUF if there was one; the
		 * timestamp is taken now either way.
		 */
		if (!was_prepared)
			vbq_take_payload(q, b, buf);
		if (q->ts_copy)
			b->timestamp = buf->timestamp;
		else
			b->timestamp.tv_sec = b->timestamp.tv_usec = 0;
	} else {
		/* A reused buffer must not carry the previous frame's stamp. */
		b->bytesused = 0;
		b->flags = 0;
		b->timestamp.tv_sec = b->timestamp.tv_usec = 0;
	}
	if (use_req) {
		error = vreq_bind(req, b);
		if (error != 0)
			goto unlock_out;
		b->state = VBUF_IN_REQUEST;
		b->request_fd = buf->request_fd;
		q->uses_requests = true;
		vbq_fill(q, b, buf);
		mtx_unlock(&q->mtx);
		error = 0;
		goto out;
	}
	q->uses_qbuf = true;
	error = vbq_enqueue_locked(q, b);
	if (error == 0) {
		mtx_lock(&q->mtx);
		vbq_fill(q, b, buf);
		mtx_unlock(&q->mtx);
	}
	return (error);

unlock_out:
	mtx_unlock(&q->mtx);
out:
	if (use_req) {
		vreq_unlock();
		vreq_put(req);
	}
	return (error);
}

/* A request was queued: its buffer b (IN_REQUEST) now goes the way of a QBUF. */
void
vbq_request_queue_buf(struct vbq *q, struct vbuf *b)
{
	int error;

	mtx_lock(&q->mtx);
	if (b->state != VBUF_IN_REQUEST) {
		mtx_unlock(&q->mtx);
		return;
	}
	error = vbq_enqueue_locked(q, b);
	if (error != 0)
		printf("vbq: starting the queue for a request failed: %d\n", error);
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
	struct vreq *req;

	if (q == NULL)
		return;
	if (!q->ts_copy && !q->is_output &&
	    b->timestamp.tv_sec == 0 && b->timestamp.tv_usec == 0)
		microuptime(&b->timestamp);
	if (b->dmat != NULL)
		bus_dmamap_sync(b->dmat, b->dmamap, BUS_DMASYNC_POSTREAD);
	mtx_lock(&q->mtx);
	if (b->state != VBUF_ACTIVE) {
		mtx_unlock(&q->mtx);
		printf("vbq: buffer %u completed while not active\n", b->index);
		return;
	}
	req = b->req;
	if (req != NULL)
		refcount_acquire(&req->refs);
	b->sequence = q->sequence++;
	b->state = error ? VBUF_ERROR : VBUF_DONE;
	if (error && !q->is_output)
		b->bytesused = 0;
	q->nactive--;
	TAILQ_INSERT_TAIL(&q->done, b, dentry);
	wakeup(&q->done);
	selwakeuppri(&q->vdev->sel, PRIBIO);
	KNOTE_LOCKED(&q->rsel.si_note, 0);
	mtx_unlock(&q->mtx);
	if (req != NULL) {
		/* The request completes with its last buffer. */
		vreq_buf_finished(req, b);
		vreq_put(req);
	}
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
	vbq_fill(q, b, buf);		/* still tells the request it was in */
	buf->flags &= ~V4L2_BUF_FLAG_DONE;	/* cleared by DQBUF; ERROR stays */
	b->state = VBUF_DEQUEUED;
	b->prepared = false;
	b->req = NULL;
	b->request_fd = 0;
	if (b->reqlist != NULL)
		vbuf_hold(b);		/* so it outlives the unbind below */
	else
		b = NULL;
	mtx_unlock(&q->mtx);
	if (b != NULL) {
		/* A dequeued buffer has left its request, which may be complete already. */
		vreq_lock();
		vreq_unbind(b);
		vreq_unlock();
		vbuf_put(b);
	}
	return (0);
}

/* -------------------------------------------------------------- poll, kqueue */

/* The events a poll on this queue is about: readable (capture) or writable (output). */
int
vbq_poll_events(struct vbq *q)
{

	return (q->is_output ? (POLLOUT | POLLWRNORM) : (POLLIN | POLLRDNORM));
}

/*
 * Can this queue ever become ready? Not if it is not streaming, has failed, or
 * has nothing queued (nothing will complete). A caller that waits on it anyway
 * would wait forever, which is what POLLERR is for.
 */
bool
vbq_poll_usable(struct vbq *q)
{
	bool usable;

	mtx_lock(&q->mtx);
	usable = q->streaming && !q->error && !TAILQ_EMPTY(&q->queued);
	mtx_unlock(&q->mtx);
	return (usable);
}

/* The events of `events` that are true now: a buffer is done and waiting. */
int
vbq_poll(struct vbq *q, int events)
{
	int want = vbq_poll_events(q), revents = 0;

	if ((events & want) == 0)
		return (0);
	mtx_lock(&q->mtx);
	if (!TAILQ_EMPTY(&q->done))
		revents = events & want;
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

static const struct filterops vbq_filterops = {
	.f_isfd = 1,
	.f_detach = vbq_kn_detach,
	.f_event = vbq_kn_event,
};

/* A capture queue is readable, an output queue writable, when a buffer is done. */
int
vbq_kqfilter(struct vbq *q, struct knote *kn)
{

	if (kn->kn_filter != (q->is_output ? EVFILT_WRITE : EVFILT_READ))
		return (EINVAL);
	kn->kn_fop = &vbq_filterops;
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
