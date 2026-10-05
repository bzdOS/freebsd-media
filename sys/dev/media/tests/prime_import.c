/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * prime_import: a V4L2 buffer becomes a GPU buffer object.
 *
 * Export a capture buffer of /dev/video0 (VIDIOC_EXPBUF), import the descriptor
 * into the GPU driver on /dev/dri/renderD128 (DRM_IOCTL_PRIME_FD_TO_HANDLE, which
 * goes through drm-kmod's dma_buf_get() and its native-dmabuf bridge), ask lima
 * for the buffer's GPU virtual address, and import it a second time.
 *
 *   cc -I<uapi> -I<shim> -I<libdrm> -I<dir of lima_drm.h> -o prime_import prime_import.c
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <drm.h>
#include "lima_drm.h"

#define	W	320
#define	H	240

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

int
main(void)
{
	struct v4l2_requestbuffers rb;
	struct v4l2_exportbuffer e;
	struct v4l2_format f;
	struct drm_prime_handle ph, ph2;
	struct drm_lima_gem_info gi;
	struct drm_gem_close gc;
	int cam, gpu;

	cam = open("/dev/video0", O_RDWR);
	gpu = open("/dev/dri/renderD128", O_RDWR);
	if (cam < 0 || gpu < 0)
		die("open");
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = W;
	f.fmt.pix.height = H;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	if (ioctl(cam, VIDIOC_S_FMT, &f) != 0)
		die("S_FMT");
	memset(&rb, 0, sizeof(rb));
	rb.count = 2;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(cam, VIDIOC_REQBUFS, &rb) != 0)
		die("REQBUFS");
	memset(&e, 0, sizeof(e));
	e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	e.index = 0;
	e.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(cam, VIDIOC_EXPBUF, &e) != 0)
		die("EXPBUF");

	memset(&ph, 0, sizeof(ph));
	ph.fd = e.fd;
	ph.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(gpu, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0)
		die("PRIME_FD_TO_HANDLE");
	printf("imported: gem handle %u\n", ph.handle);

	memset(&gi, 0, sizeof(gi));
	gi.handle = ph.handle;
	if (ioctl(gpu, DRM_IOCTL_LIMA_GEM_INFO, &gi) != 0)
		die("LIMA_GEM_INFO");
	printf("lima: gpu va 0x%x (buffer is %d bytes)\n", gi.va, W * H * 2);

	memset(&ph2, 0, sizeof(ph2));
	ph2.fd = e.fd;
	ph2.flags = DRM_CLOEXEC | DRM_RDWR;
	if (ioctl(gpu, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph2) != 0)
		die("second PRIME_FD_TO_HANDLE");
	printf("imported again: gem handle %u\n", ph2.handle);

	memset(&gc, 0, sizeof(gc));
	gc.handle = ph.handle;
	if (ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &gc) != 0)
		die("GEM_CLOSE");
	if (ph2.handle != ph.handle) {
		gc.handle = ph2.handle;
		if (ioctl(gpu, DRM_IOCTL_GEM_CLOSE, &gc) != 0)
			die("GEM_CLOSE 2");
	}
	printf("closed; ok\n");
	return (0);
}
