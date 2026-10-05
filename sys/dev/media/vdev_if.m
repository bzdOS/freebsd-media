#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Video device methods. A driver implements the ones its device has; every
# other ioctl answers ENOTTY.
#

#include <dev/media/media.h>

INTERFACE vdev;

HEADER {
	#include <sys/types.h>
	#include <dev/media/uapi/linux/videodev2.h>

	struct vdev;
	struct vctrl;
}

CODE {
	static int
	vdev_default_notty(void)
	{

		return (ENOTTY);
	}
};

# Fill driver, card and bus_info; the core fills version and the capability
# words from the device caps.
METHOD int querycap {
	struct vdev		*v;
	struct v4l2_capability	*cap;
};

# Walk the formats: f->index selects, EINVAL ends the list. The core has set
# f->type to a type the device supports.
METHOD int enum_fmt {
	struct vdev		*v;
	struct v4l2_fmtdesc	*f;
} DEFAULT vdev_default_notty;

METHOD int g_fmt {
	struct vdev		*v;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

# Adjust *f to the nearest format the device supports, without changing state.
METHOD int try_fmt {
	struct vdev		*v;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

# try_fmt, then make it current. The core refuses it with EBUSY while buffers
# are allocated.
METHOD int s_fmt {
	struct vdev		*v;
	struct v4l2_format	*f;
} DEFAULT vdev_default_notty;

METHOD int enum_framesizes {
	struct vdev			*v;
	struct v4l2_frmsizeenum		*f;
} DEFAULT vdev_default_notty;

METHOD int enum_frameintervals {
	struct vdev			*v;
	struct v4l2_frmivalenum		*f;
} DEFAULT vdev_default_notty;

METHOD int g_parm {
	struct vdev		*v;
	struct v4l2_streamparm	*p;
} DEFAULT vdev_default_notty;

METHOD int s_parm {
	struct vdev		*v;
	struct v4l2_streamparm	*p;
} DEFAULT vdev_default_notty;

# The control's value changed; apply it to the hardware. May sleep. `c->val`
# holds the new value; returning non-zero leaves the old one in place.
METHOD int s_ctrl {
	struct vdev		*v;
	struct vctrl		*c;
};
