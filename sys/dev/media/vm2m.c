/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The mem2mem scheduler (see vm2m.h).
 *
 * A context is ready to run when both of its queues are streaming and each has
 * a buffer waiting. Ready contexts line up on dev->jobs; one job runs at a
 * time per device, started from a taskqueue so that device_run may sleep and a
 * driver that finishes inside device_run does not recurse.
 *
 * dev->lock protects the job list, `curr`, and the ready lists of every
 * context of the device. It is never held while calling the driver, and is
 * taken inside nothing but the queue lock (vbq_buf_done() is called without
 * it).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/condvar.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>

#include <dev/media/vm2m.h>

static void vm2m_run_task(void *, int);

void
vm2m_dev_init(struct vm2m_dev *dev, kobj_class_t cls)
{

	kobj_init((kobj_t)dev, cls);
	mtx_init(&dev->lock, "vm2m", NULL, MTX_DEF);
	cv_init(&dev->finished, "vm2mfin");
	TAILQ_INIT(&dev->jobs);
	dev->curr = NULL;
	TASK_INIT(&dev->run_task, 0, vm2m_run_task, dev);
}

void
vm2m_dev_destroy(struct vm2m_dev *dev)
{

	taskqueue_drain(taskqueue_thread, &dev->run_task);
	cv_destroy(&dev->finished);
	mtx_destroy(&dev->lock);
	kobj_delete((kobj_t)dev, NULL);
}

void
vm2m_ctx_init(struct vm2m_ctx *ctx, struct vm2m_dev *dev, struct vbq *out,
    struct vbq *cap)
{

	ctx->dev = dev;
	ctx->out = out;
	ctx->cap = cap;
	TAILQ_INIT(&ctx->out_rdy);
	TAILQ_INIT(&ctx->cap_rdy);
	ctx->queued = false;
}

/*
 * Wait for ctx's running job, if any, to end, and make sure ctx is not left on
 * the job list (a finishing job puts its context back if more is waiting).
 */
static void
vm2m_cancel_job(struct vm2m_ctx *ctx)
{
	struct vm2m_dev *dev = ctx->dev;

	mtx_lock(&dev->lock);
	if (dev->curr == ctx) {
		mtx_unlock(&dev->lock);
		VM2M_JOB_ABORT(dev, ctx);
		mtx_lock(&dev->lock);
		while (dev->curr == ctx)
			cv_wait(&dev->finished, &dev->lock);
	}
	if (ctx->queued) {
		TAILQ_REMOVE(&dev->jobs, ctx, jobentry);
		ctx->queued = false;
	}
	mtx_unlock(&dev->lock);
}

void
vm2m_ctx_destroy(struct vm2m_ctx *ctx)
{

	vm2m_cancel_job(ctx);
	mtx_lock(&ctx->dev->lock);
	TAILQ_INIT(&ctx->out_rdy);
	TAILQ_INIT(&ctx->cap_rdy);
	mtx_unlock(&ctx->dev->lock);
}

/* ---------------------------------------------------------------- scheduling */

/* Start the next job if none is running. dev->lock is held. */
static void
vm2m_try_run_locked(struct vm2m_dev *dev)
{
	struct vm2m_ctx *ctx;

	mtx_assert(&dev->lock, MA_OWNED);
	if (dev->curr != NULL)
		return;
	ctx = TAILQ_FIRST(&dev->jobs);
	if (ctx == NULL)
		return;
	TAILQ_REMOVE(&dev->jobs, ctx, jobentry);
	ctx->queued = false;
	dev->curr = ctx;
	taskqueue_enqueue(taskqueue_thread, &dev->run_task);
}

static void
vm2m_run_task(void *arg, int pending)
{
	struct vm2m_dev *dev = arg;
	struct vm2m_ctx *ctx;

	mtx_lock(&dev->lock);
	ctx = dev->curr;
	mtx_unlock(&dev->lock);
	if (ctx != NULL)
		VM2M_DEVICE_RUN(dev, ctx);
}

/* Both queues streaming and a buffer waiting in each? dev->lock is NOT held. */
static bool
vm2m_ctx_has_work(struct vm2m_ctx *ctx)
{
	bool work;

	if (!vbq_is_streaming(ctx->out) || !vbq_is_streaming(ctx->cap))
		return (false);
	mtx_lock(&ctx->dev->lock);
	work = !TAILQ_EMPTY(&ctx->out_rdy) && !TAILQ_EMPTY(&ctx->cap_rdy);
	mtx_unlock(&ctx->dev->lock);
	return (work);
}

/* Queue ctx for a job if it has what one needs. dev->lock is NOT held. */
static void
vm2m_try_schedule(struct vm2m_ctx *ctx)
{
	struct vm2m_dev *dev = ctx->dev;

	if (!vm2m_ctx_has_work(ctx) || !VM2M_JOB_READY(dev, ctx))
		return;
	mtx_lock(&dev->lock);
	if (!ctx->queued && dev->curr != ctx) {
		TAILQ_INSERT_TAIL(&dev->jobs, ctx, jobentry);
		ctx->queued = true;
	}
	vm2m_try_run_locked(dev);
	mtx_unlock(&dev->lock);
}

void
vm2m_buf_queue(struct vm2m_ctx *ctx, struct vbuf *b)
{
	struct vm2m_dev *dev = ctx->dev;

	mtx_lock(&dev->lock);
	TAILQ_INSERT_TAIL(b->q == ctx->out ? &ctx->out_rdy : &ctx->cap_rdy, b, drvq);
	mtx_unlock(&dev->lock);
	vm2m_try_schedule(ctx);
}

void
vm2m_stop_streaming(struct vm2m_ctx *ctx, struct vbq *q)
{
	struct vm2m_dev *dev = ctx->dev;

	/* Let the running job end before its buffers are taken away. */
	vm2m_cancel_job(ctx);
	mtx_lock(&dev->lock);
	if (q == ctx->out)
		TAILQ_INIT(&ctx->out_rdy);
	else
		TAILQ_INIT(&ctx->cap_rdy);
	mtx_unlock(&dev->lock);
}

struct vbuf *
vm2m_next_src(struct vm2m_ctx *ctx)
{
	struct vbuf *b;

	mtx_lock(&ctx->dev->lock);
	b = TAILQ_FIRST(&ctx->out_rdy);
	mtx_unlock(&ctx->dev->lock);
	return (b);
}

struct vbuf *
vm2m_next_dst(struct vm2m_ctx *ctx)
{
	struct vbuf *b;

	mtx_lock(&ctx->dev->lock);
	b = TAILQ_FIRST(&ctx->cap_rdy);
	mtx_unlock(&ctx->dev->lock);
	return (b);
}

void
vm2m_buf_done_and_job_finish(struct vm2m_ctx *ctx, bool error)
{
	struct vm2m_dev *dev = ctx->dev;
	struct vbuf *src, *dst;
	bool more;

	mtx_lock(&dev->lock);
	src = TAILQ_FIRST(&ctx->out_rdy);
	dst = TAILQ_FIRST(&ctx->cap_rdy);
	if (src != NULL)
		TAILQ_REMOVE(&ctx->out_rdy, src, drvq);
	if (dst != NULL)
		TAILQ_REMOVE(&ctx->cap_rdy, dst, drvq);
	mtx_unlock(&dev->lock);

	if (src != NULL && dst != NULL) {
		/* The result is the source's frame: same stamp and frame type. */
		dst->timestamp = src->timestamp;
		dst->flags = (dst->flags & ~VBQ_FRAME_TYPE_FLAGS) |
		    (src->flags & VBQ_FRAME_TYPE_FLAGS);
	}
	if (dst != NULL)
		vbq_buf_done(dst, error);
	if (src != NULL)
		vbq_buf_done(src, error);

	/*
	 * ctx stays `curr` until the very end, so vm2m_cancel_job() waits for us
	 * and ctx cannot be destroyed under us. Decide now whether it has more to
	 * do, put it back on the job list if so, and only then let go of it.
	 */
	more = vm2m_ctx_has_work(ctx) && VM2M_JOB_READY(dev, ctx);
	mtx_lock(&dev->lock);
	if (more) {
		TAILQ_INSERT_TAIL(&dev->jobs, ctx, jobentry);
		ctx->queued = true;
	}
	dev->curr = NULL;
	cv_broadcast(&dev->finished);
	vm2m_try_run_locked(dev);
	mtx_unlock(&dev->lock);
}
