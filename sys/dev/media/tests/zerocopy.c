/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * zerocopy: camera -> converter through a dmabuf, no copy in between.
 *
 * The camera (/dev/video0, vtest) exports its capture buffers with VIDIOC_EXPBUF;
 * the converter (/dev/video1, vconv) takes them as its OUTPUT queue with
 * V4L2_MEMORY_DMABUF and turns YUYV into RGB24 in its own capture buffers. The
 * converter's source buffers have no memory of their own, so every pixel it
 * converts was read from the camera's buffer. The result is checked against
 * the colour bars vtest draws.
 *
 *   cc -I<dir with linux/videodev2.h> -o zerocopy zerocopy.c && ./zerocopy [frames]
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define	W	320
#define	H	240
#define	NBUF	4

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

static void
xioctl(int fd, unsigned long req, void *arg, const char *what)
{

	if (ioctl(fd, req, arg) != 0)
		die(what);
}

static void
setfmt(int fd, unsigned type, unsigned pixfmt)
{
	struct v4l2_format f;

	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix.width = W;
	f.fmt.pix.height = H;
	f.fmt.pix.pixelformat = pixfmt;
	xioctl(fd, VIDIOC_S_FMT, &f, "S_FMT");
	if (f.fmt.pix.width != W || f.fmt.pix.height != H)
		die("format was not accepted as asked");
}

static void
reqbufs(int fd, unsigned type, unsigned memory)
{
	struct v4l2_requestbuffers rb;

	memset(&rb, 0, sizeof(rb));
	rb.count = NBUF;
	rb.type = type;
	rb.memory = memory;
	xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS");
	if (rb.count < NBUF)
		die("too few buffers");
}

static void
wait_readable(int fd, short ev)
{
	struct pollfd p = { .fd = fd, .events = ev };

	if (poll(&p, 1, 5000) != 1 || (p.revents & POLLERR))
		die("poll");
}

int
main(int argc, char **argv)
{
	/* bars as vtest draws them: white yellow cyan green magenta red blue black */
	static const unsigned char bars[8][3] = {
		{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
		{255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
	};
	int frames = argc > 1 ? atoi(argv[1]) : 10;
	int cam, conv, dmafd[NBUF], bad = 0, f, i, k;
	unsigned char *rgb[NBUF];
	size_t rgblen = 0;
	struct v4l2_buffer b;
	unsigned type;

	cam = open("/dev/video0", O_RDWR);
	conv = open("/dev/video1", O_RDWR);
	if (cam < 0 || conv < 0)
		die("open");

	/* camera: YUYV, buffers we can export */
	setfmt(cam, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_YUYV);
	reqbufs(cam, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP);
	for (i = 0; i < NBUF; i++) {
		struct v4l2_exportbuffer e;

		memset(&e, 0, sizeof(e));
		e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		e.index = i;
		e.flags = O_CLOEXEC | O_RDWR;
		xioctl(cam, VIDIOC_EXPBUF, &e, "EXPBUF");
		dmafd[i] = e.fd;
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		xioctl(cam, VIDIOC_QBUF, &b, "cam QBUF");
	}

	/* converter: YUYV from dmabufs in, RGB24 out */
	setfmt(conv, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_YUYV);
	setfmt(conv, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_RGB24);
	reqbufs(conv, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF);
	reqbufs(conv, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP);
	for (i = 0; i < NBUF; i++) {
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		xioctl(conv, VIDIOC_QUERYBUF, &b, "conv QUERYBUF");
		rgblen = b.length;
		rgb[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
		    conv, b.m.offset);
		if (rgb[i] == MAP_FAILED)
			die("mmap result buffer");
		xioctl(conv, VIDIOC_QBUF, &b, "conv QBUF capture");
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(conv, VIDIOC_STREAMON, &type, "conv STREAMON capture");
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(conv, VIDIOC_STREAMON, &type, "conv STREAMON output");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(cam, VIDIOC_STREAMON, &type, "cam STREAMON");

	for (f = 0; f < frames; f++) {
		int ci, oi;

		wait_readable(cam, POLLIN);
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		xioctl(cam, VIDIOC_DQBUF, &b, "cam DQBUF");
		ci = b.index;

		/* hand the camera's buffer to the converter: same memory, by descriptor */
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		b.memory = V4L2_MEMORY_DMABUF;
		b.index = f % NBUF;
		b.m.fd = dmafd[ci];
		b.bytesused = W * H * 2;
		gettimeofday(&b.timestamp, NULL);
		xioctl(conv, VIDIOC_QBUF, &b, "conv QBUF output (dmabuf)");

		wait_readable(conv, POLLIN);
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		xioctl(conv, VIDIOC_DQBUF, &b, "conv DQBUF capture");
		oi = b.index;
		if (b.flags & V4L2_BUF_FLAG_ERROR)
			bad++;
		for (k = 0; k < 8; k++) {	/* the middle of each bar, row 120 */
			const unsigned char *p = rgb[oi] + ((size_t)120 * W + (k * 2 + 1) * W / 16) * 3;

			if (abs(p[0] - bars[k][0]) > 3 || abs(p[1] - bars[k][1]) > 3 ||
			    abs(p[2] - bars[k][2]) > 3) {
				printf("frame %d bar %d: got %u %u %u, want %u %u %u\n", f, k,
				    p[0], p[1], p[2], bars[k][0], bars[k][1], bars[k][2]);
				bad++;
			}
		}
		xioctl(conv, VIDIOC_QBUF, &b, "conv QBUF capture again");

		memset(&b, 0, sizeof(b));	/* give the source buffer back */
		b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		b.memory = V4L2_MEMORY_DMABUF;
		wait_readable(conv, POLLOUT);
		xioctl(conv, VIDIOC_DQBUF, &b, "conv DQBUF output");
		if (b.m.fd != dmafd[ci])
			die("dequeued output buffer reports another descriptor");
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = ci;
		xioctl(cam, VIDIOC_QBUF, &b, "cam QBUF again");
	}
	printf("%d frames camera -> converter through dmabuf, %zu bytes each: %s\n",
	    frames, rgblen, bad == 0 ? "all bars correct" : "MISMATCH");
	return (bad != 0);
}
