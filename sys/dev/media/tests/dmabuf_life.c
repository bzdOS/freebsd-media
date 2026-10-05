/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dmabuf_life: an exported buffer outlives its queue and its device.
 *
 * Export a capture buffer of /dev/video0, map the descriptor, free the queue
 * (REQBUFS 0 must succeed with the descriptor and the mapping alive: orphaned
 * buffers), close the device, and check the memory is still there and usable.
 * Then hold it for `secs` seconds so the caller can try to unload modules.
 *
 *   cc -I<uapi> -I<shim> -o dmabuf_life dmabuf_life.c && ./dmabuf_life [secs]
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

int
main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 0;
	struct v4l2_requestbuffers rb;
	struct v4l2_exportbuffer e;
	struct v4l2_format f;
	unsigned char *p;
	off_t size;
	size_t i;
	int cam, fd;

	cam = open("/dev/video0", O_RDWR);
	if (cam < 0)
		die("open");
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = 320;
	f.fmt.pix.height = 240;
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
	e.index = 1;
	e.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(cam, VIDIOC_EXPBUF, &e) != 0)
		die("EXPBUF");
	fd = e.fd;
	size = lseek(fd, 0, SEEK_END);
	if (size != 320 * 240 * 2) {
		printf("lseek(SEEK_END) says %lld, want %d\n", (long long)size, 320 * 240 * 2);
		return (1);
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		die("mmap of the descriptor");
	memset(p, 0x5a, size);

	rb.count = 0;
	if (ioctl(cam, VIDIOC_REQBUFS, &rb) != 0)
		die("REQBUFS(0) with the buffer exported and mapped");
	close(cam);
	for (i = 0; i < (size_t)size; i += 4096)
		if (p[i] != 0x5a) {
			printf("memory changed after the queue was freed (at %zu)\n", i);
			return (1);
		}
	memset(p, 0xa5, size);		/* still writable */
	printf("orphaned buffer ok: %lld bytes, queue freed, device closed\n", (long long)size);
	fflush(stdout);
	sleep(secs);
	munmap(p, size);
	close(fd);
	return (0);
}
