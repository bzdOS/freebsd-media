#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Buffer queue methods a driver implements. The semantics follow Linux's
# vb2_ops on purpose (the method names and call order are the ones V4L2 driver
# authors know; no Linux code is used), but the core validates everything first.
#

#include <dev/media/media.h>

INTERFACE vbq;

HEADER {
	#include <sys/types.h>

	struct vbq;
	struct vbuf;
}

CODE {
	static int
	vbq_default_prepare(struct vbq *q, struct vbuf *b)
	{

		return (0);
	}

	static int
	vbq_default_start(struct vbq *q, uint32_t count)
	{

		return (0);
	}

	static void
	vbq_default_stop(struct vbq *q)
	{
	}

	static int
	vbq_default_alloc_mem(struct vbq *q, struct vbuf *b, size_t size)
	{

		return (EOPNOTSUPP);
	}
};

#
# REQBUFS asks for *nbuffers buffers. The driver may change *nbuffers (it will
# be clamped to 1..VBQ_MAX_BUFFERS) and must set *size to the size of one
# buffer, normally the current format's sizeimage.
#
METHOD int queue_setup {
	struct vbq	*q;
	uint32_t	*nbuffers;
	size_t		*size;
};

#
# Called on QBUF/PREPARE_BUF. The buffer's size is already known to be right.
#
METHOD int buf_prepare {
	struct vbq	*q;
	struct vbuf	*b;
} DEFAULT vbq_default_prepare;

#
# The buffer is the driver's until it calls vbq_buf_done(). Called with no
# queue lock held; the driver must not call vbq_buf_done() from inside it
# unless the lock it passes in has been dropped (it has: this is plain code).
#
METHOD void buf_queue {
	struct vbq	*q;
	struct vbuf	*b;
};

#
# Called when STREAMON is seen and at least min_queued buffers are queued
# (possibly later, from QBUF). `count` buffers are already queued. If it fails,
# the core puts the buffers back to QUEUED.
#
METHOD int start_streaming {
	struct vbq	*q;
	uint32_t	count;
} DEFAULT vbq_default_start;

#
# Stop the hardware. Buffers it still holds do not need to be handed back: the
# core takes every buffer back afterwards, so once this returns the device must
# no longer touch any of them.
#
METHOD void stop_streaming {
	struct vbq	*q;
} DEFAULT vbq_default_stop;

#
# Optional: memory for one buffer from the driver's own allocator (a reserved
# pool, say) instead of the core's bus_dma. `size` is page aligned. On success
# the driver sets b->vaddr (kernel mapping), b->paddr (physically contiguous),
# b->memattr and b->mem_free (called with the buffer when its last reference
# goes; it may be after the queue itself is gone, so it must not use b->q).
# Any error, EOPNOTSUPP included, makes the core fall back to bus_dma.
#
METHOD int alloc_mem {
	struct vbq	*q;
	struct vbuf	*b;
	size_t		size;
} DEFAULT vbq_default_alloc_mem;
