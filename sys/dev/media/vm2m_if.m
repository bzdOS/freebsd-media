#-
# SPDX-License-Identifier: BSD-2-Clause
#
# What a mem2mem driver provides to the scheduler in vm2m.c. A job is one
# source buffer turned into one destination buffer.
#

#include <dev/media/vm2m.h>

INTERFACE vm2m;

HEADER {
	struct vm2m_dev;
	struct vm2m_ctx;
}

CODE {
	static int
	vm2m_default_ready(struct vm2m_dev *dev, struct vm2m_ctx *ctx)
	{

		return (1);
	}

	static void
	vm2m_default_abort(struct vm2m_dev *dev, struct vm2m_ctx *ctx)
	{
	}
};

#
# (Every method takes the device first: that is the object the call is
# dispatched on. The context says which open file the job is for.)
#
# Run one job: vm2m_next_src() and vm2m_next_dst() are its buffers. Called from
# a taskqueue thread, so it may sleep. The job ends when the driver calls
# vm2m_buf_done_and_job_finish(), from here or later from an interrupt.
#
METHOD void device_run {
	struct vm2m_dev	*dev;
	struct vm2m_ctx	*ctx;
};

#
# Is there enough to run a job now? The scheduler already knows that a source
# and a destination buffer are ready; a driver that needs more (several sources,
# a minimum size) says so here.
#
METHOD int job_ready {
	struct vm2m_dev	*dev;
	struct vm2m_ctx	*ctx;
} DEFAULT vm2m_default_ready;

#
# The running job has to stop soon (STREAMOFF, close). It must still end with
# vm2m_buf_done_and_job_finish(); the caller waits for that.
#
METHOD void job_abort {
	struct vm2m_dev	*dev;
	struct vm2m_ctx	*ctx;
} DEFAULT vm2m_default_abort;
