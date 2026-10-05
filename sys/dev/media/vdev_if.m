#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Video device methods. A driver implements the ones its device has; every
# other ioctl answers ENOTTY.
#
# Every method takes the open file (`fh`) the call comes from. A device with
# one shared queue ignores it; a mem2mem device keeps its formats, queues and
# controls per open file, hung off fh->priv, and finds them through it.
#

#include <dev/media/media.h>

INTERFACE vdev;

HEADER {
	#include <sys/types.h>
	#include <dev/media/uapi/linux/videodev2.h>

	struct vdev;
	struct vfh;
	struct vctrl;
}

CODE {
	static int
	vdev_default_notty(void)
	{

		return (ENOTTY);
	}

	static int
	vdev_default_ok(void)
	{

		return (0);
	}

	static void
	vdev_default_void(void)
	{
	}
};

# A file is being opened. A device with per-open state (mem2mem) allocates it
# here and sets fh->priv, fh->capq, fh->outq and fh->ctrl_handler; the core has
# already pointed those at the device's shared queue and handler, if any.
METHOD int fh_open {
	struct vdev		*v;
	struct vfh		*fh;
} DEFAULT vdev_default_ok;

# The file is closed (or the device went away). The core has stopped and freed
# the queues in fh->capq/outq before this; release what fh_open made.
METHOD void fh_close {
	struct vdev		*v;
	struct vfh		*fh;
} DEFAULT vdev_default_void;

# Fill driver, card and bus_info; the core fills version and the capability
# words from the device caps.
METHOD int querycap {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_capability	*cap;
};

# Walk the formats: f->index selects, EINVAL ends the list. The core has set
# f->type to a type the device has a queue for.
METHOD int enum_fmt {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_fmtdesc	*f;
} DEFAULT vdev_default_notty;

METHOD int g_fmt {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

# Adjust *f to the nearest format the device supports, without changing state.
METHOD int try_fmt {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

# try_fmt, then make it current. The core refuses it with EBUSY while buffers
# of that queue are allocated.
METHOD int s_fmt {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

METHOD int enum_framesizes {
	struct vdev			*v;
	struct vfh			*fh;
	struct v4l2_frmsizeenum		*f;
} DEFAULT vdev_default_notty;

METHOD int enum_frameintervals {
	struct vdev			*v;
	struct vfh			*fh;
	struct v4l2_frmivalenum		*f;
} DEFAULT vdev_default_notty;

METHOD int g_parm {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_streamparm	*p;
} DEFAULT vdev_default_notty;

METHOD int s_parm {
	struct vdev		*v;
	struct vfh		*fh;
	struct v4l2_streamparm	*p;
} DEFAULT vdev_default_notty;

# The control's value changed; apply it to the hardware. May sleep. `c->val`
# holds the new value; returning non-zero leaves the old one in place. `fh` is
# the file that asked.
METHOD int s_ctrl {
	struct vdev		*v;
	struct vfh		*fh;
	struct vctrl		*c;
};
