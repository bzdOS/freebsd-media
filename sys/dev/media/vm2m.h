/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Memory-to-memory devices: a scheduler that pairs a source buffer from an
 * OUTPUT queue with a destination buffer from a CAPTURE queue and runs the
 * driver on them, one job at a time per device.
 *
 * A driver embeds a struct vm2m_dev in its softc (one per device) and a struct
 * vm2m_ctx in its per-open context (one per open file), next to that file's two
 * queues. Its buf_queue methods call vm2m_buf_queue(); its stop_streaming
 * methods call vm2m_stop_streaming(); device_run (vm2m_if.m) does the work and
 * ends with vm2m_buf_done_and_job_finish().
 */
#ifndef _DEV_MEDIA_VM2M_H_
#define	_DEV_MEDIA_VM2M_H_

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/condvar.h>
#include <sys/kobj.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

#include <dev/media/media.h>

#include "vm2m_if.h"

struct vm2m_ctx;

struct vm2m_dev {
	KOBJ_FIELDS;
	struct mtx		lock;
	struct cv		finished;
	TAILQ_HEAD(, vm2m_ctx)	jobs;		/* contexts ready to run */
	struct vm2m_ctx		*curr;		/* the running job, or NULL */
	struct task		run_task;
};

struct vm2m_ctx {
	struct vm2m_dev		*dev;
	struct vbq		*out, *cap;
	TAILQ_HEAD(, vbuf)	out_rdy, cap_rdy;	/* given to us, oldest first */
	TAILQ_ENTRY(vm2m_ctx)	jobentry;
	bool			queued;		/* on dev->jobs */
	bool			orphan;		/* a held picture ended in error: its remaining slices have no destination */
};

/* `cls` is the driver's vm2m_if class. */
void vm2m_dev_init(struct vm2m_dev *, kobj_class_t cls);
void vm2m_dev_destroy(struct vm2m_dev *);
void vm2m_ctx_init(struct vm2m_ctx *, struct vm2m_dev *, struct vbq *out,
    struct vbq *cap);
/* Cancels a running or queued job and forgets the buffers. */
void vm2m_ctx_destroy(struct vm2m_ctx *);

/* From the vbq buf_queue method: the buffer is ours until a job completes it. */
void vm2m_buf_queue(struct vm2m_ctx *, struct vbuf *);
/* From the vbq stop_streaming method of either queue. */
void vm2m_stop_streaming(struct vm2m_ctx *, struct vbq *);

/* The buffers of the job being run (or the next one): oldest ready, not removed. */
struct vbuf *vm2m_next_src(struct vm2m_ctx *);
struct vbuf *vm2m_next_dst(struct vm2m_ctx *);

/*
 * End the running job: complete the source and destination buffers (the
 * destination takes the source's timestamp and frame type), then start the next
 * job. Set dst->bytesused first.
 */
void vm2m_buf_done_and_job_finish(struct vm2m_ctx *, bool error);

#endif /* _KERNEL */

#endif /* _DEV_MEDIA_VM2M_H_ */
