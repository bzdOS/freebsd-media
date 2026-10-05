#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Buffer queue methods a driver implements. The semantics follow Linux's
# vb2_ops on purpose (a Linux driver is the hardware reference), but the core
# validates everything first.
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
# (possibly later, from QBUF). `count` buffers are already queued. On failure
# the driver must hand every buffer it got back with vbq_return_active().
#
METHOD int start_streaming {
	struct vbq	*q;
	uint32_t	count;
} DEFAULT vbq_default_start;

#
# Stop the hardware and give every active buffer back (vbq_return_active()).
#
METHOD void stop_streaming {
	struct vbq	*q;
} DEFAULT vbq_default_stop;
