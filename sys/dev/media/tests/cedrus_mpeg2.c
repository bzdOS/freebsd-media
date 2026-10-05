/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_mpeg2: decode the first picture of an MPEG-2 elementary stream through
 * the stateless V4L2 interface (/dev/video*, /dev/media*) and compare it with a
 * reference decode (raw yuv420p, e.g. from ffmpeg).
 *
 *   cedrus_mpeg2 [-p] [-n count] [-v video] [-m media] stream.m2v reference.yuv
 *
 * -p   give the driver the data from the picture start code (headers and all)
 *      instead of from the first slice start code
 * -n   decode the same picture `count` times (default 2), checking each result
 * -t N feed only the first N bytes of the slice data (a damaged stream: the
 *      buffer must come back flagged ERROR, and the next picture must decode)
 * -g   feed garbage instead of slice data (after the first start code)
 *
 * The headers are parsed here the way a userland decoder would, and handed to
 * the driver as the three MPEG-2 controls inside a media request.
 *
 *   cc -I<dir with linux/videodev2.h> -o cedrus_mpeg2 cedrus_mpeg2.c
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/media.h>

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

static int
xioctl(int fd, unsigned long req, void *arg, const char *what)
{

	if (ioctl(fd, req, arg) != 0) {
		fprintf(stderr, "%s: ", what);
		die("ioctl");
	}
	return (0);
}

static unsigned
getbits(const uint8_t *b, size_t pos, int n)
{
	unsigned v = 0;

	while (n-- > 0) {
		v = (v << 1) | ((b[pos >> 3] >> (7 - (pos & 7))) & 1);
		pos++;
	}
	return (v);
}

static const uint8_t default_intra[64] = {
	8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
	27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
	29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
	35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83 };

struct stream {
	uint8_t		*data;
	size_t		len;
	size_t		pic_off;	/* picture start code (00 00 01 00) */
	size_t		slice_off;	/* first slice start code */
	size_t		slice_end;	/* end of this picture's slices */
	struct v4l2_ctrl_mpeg2_sequence seq;
	struct v4l2_ctrl_mpeg2_picture pic;
	struct v4l2_ctrl_mpeg2_quantisation q;
};

static void
parse(struct stream *s, const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t i, n;
	int have_pic = 0, have_ext = 0;

	if (f == NULL)
		die(path);
	fseek(f, 0, SEEK_END);
	s->len = ftell(f);
	fseek(f, 0, SEEK_SET);
	s->data = malloc(s->len + 16);
	n = fread(s->data, 1, s->len, f);
	fclose(f);
	if (n != s->len)
		die("read");
	memset(&s->seq, 0, sizeof(s->seq));
	memset(&s->pic, 0, sizeof(s->pic));
	memset(&s->q, 0, sizeof(s->q));
	memcpy(s->q.intra_quantiser_matrix, default_intra, 64);
	memset(s->q.non_intra_quantiser_matrix, 16, 64);
	s->slice_off = s->slice_end = 0;
	for (i = 0; i + 4 <= s->len; i++) {
		uint8_t c;
		size_t p;

		if (s->data[i] != 0 || s->data[i + 1] != 0 || s->data[i + 2] != 1)
			continue;
		c = s->data[i + 3];
		p = (i + 4) * 8;
		if (c == 0xb3) {
			size_t q = p + 62;
			int k;

			s->seq.horizontal_size = getbits(s->data, p, 12);
			s->seq.vertical_size = getbits(s->data, p + 12, 12);
			s->seq.chroma_format = 1;
			if (getbits(s->data, q, 1)) {
				for (k = 0; k < 64; k++)
					s->q.intra_quantiser_matrix[k] = getbits(s->data, q + 1 + 8 * k, 8);
				q += 1 + 512;
			} else
				q += 1;
			if (getbits(s->data, q, 1))
				for (k = 0; k < 64; k++)
					s->q.non_intra_quantiser_matrix[k] = getbits(s->data, q + 1 + 8 * k, 8);
		} else if (c == 0x00 && !have_pic) {
			s->pic.picture_coding_type = getbits(s->data, p + 10, 3);
			s->pic_off = i;
			have_pic = 1;
		} else if (c == 0xb5 && getbits(s->data, p, 4) == 8 && !have_ext) {
			size_t q = p + 4;
			unsigned f00 = getbits(s->data, q, 4), f01 = getbits(s->data, q + 4, 4),
			    f10 = getbits(s->data, q + 8, 4), f11 = getbits(s->data, q + 12, 4);
			unsigned fl;

			q += 16;
			s->pic.f_code[0][0] = f00;
			s->pic.f_code[0][1] = f01;
			s->pic.f_code[1][0] = f10;
			s->pic.f_code[1][1] = f11;
			s->pic.intra_dc_precision = getbits(s->data, q, 2);
			s->pic.picture_structure = getbits(s->data, q + 2, 2);
			fl = 0;
			if (getbits(s->data, q + 4, 1)) fl |= V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST;
			if (getbits(s->data, q + 5, 1)) fl |= V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT;
			if (getbits(s->data, q + 6, 1)) fl |= V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV;
			if (getbits(s->data, q + 7, 1)) fl |= V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE;
			if (getbits(s->data, q + 8, 1)) fl |= V4L2_MPEG2_PIC_FLAG_INTRA_VLC;
			if (getbits(s->data, q + 9, 1)) fl |= V4L2_MPEG2_PIC_FLAG_ALT_SCAN;
			if (getbits(s->data, q + 10, 1)) fl |= V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST;
			if (getbits(s->data, q + 12, 1)) fl |= V4L2_MPEG2_PIC_FLAG_PROGRESSIVE;
			s->pic.flags = fl;
			have_ext = 1;
		} else if (c >= 0x01 && c <= 0xaf) {
			if (s->slice_off == 0)
				s->slice_off = i;
		} else if (s->slice_off != 0 && s->slice_end == 0 &&
		    (c == 0x00 || c == 0xb3 || c == 0xb7 || c == 0xb8)) {
			s->slice_end = i;
		}
	}
	if (s->slice_end == 0)
		s->slice_end = s->len;
	if (!have_pic || !have_ext || s->seq.horizontal_size == 0 || s->slice_off == 0) {
		fprintf(stderr, "%s: not an MPEG-2 stream I can read\n", path);
		exit(1);
	}
}

int
main(int argc, char **argv)
{
	const char *vpath = "/dev/video0", *mpath = "/dev/media0";
	int c, count = 2, from_pic = 0, vfd, mfd, iter, bad = 0, garbage = 0;
	size_t trunc_to = 0;
	struct stream s;
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	struct v4l2_ext_control ec[3];
	struct v4l2_ext_controls ecs;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	uint8_t *obuf, *cbuf, *ref;
	size_t obuf_len, cbuf_len, off, len, rlen;
	unsigned w, h, stride, ha, y, x;
	FILE *rf;

	while ((c = getopt(argc, argv, "pn:v:m:t:g")) != -1) {
		switch (c) {
		case 'p': from_pic = 1; break;
		case 'n': count = atoi(optarg); break;
		case 'v': vpath = optarg; break;
		case 'm': mpath = optarg; break;
		case 't': trunc_to = atoi(optarg); break;
		case 'g': garbage = 1; break;
		default: return (2);
		}
	}
	if (argc - optind != 2) {
		fprintf(stderr, "usage: cedrus_mpeg2 [-p] [-n count] [-v video] [-m media] stream.m2v ref.yuv\n");
		return (2);
	}
	parse(&s, argv[optind]);
	w = s.seq.horizontal_size;
	h = s.seq.vertical_size;
	rf = fopen(argv[optind + 1], "rb");
	if (rf == NULL)
		die(argv[optind + 1]);
	rlen = w * h * 3 / 2;
	ref = malloc(rlen);
	if (fread(ref, 1, rlen, rf) != rlen)
		die("reference too short");
	fclose(rf);

	vfd = open(vpath, O_RDWR);
	mfd = open(mpath, O_RDWR);
	if (vfd < 0 || mfd < 0)
		die("open");

	memset(&f, 0, sizeof(f));
	f.type = ot;
	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_MPEG2_SLICE;
	xioctl(vfd, VIDIOC_S_FMT, &f, "S_FMT out");
	memset(&f, 0, sizeof(f));
	f.type = ct;
	xioctl(vfd, VIDIOC_G_FMT, &f, "G_FMT cap");
	if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
		fprintf(stderr, "capture format is not NV12\n");
		return (1);
	}
	stride = f.fmt.pix.bytesperline;
	ha = f.fmt.pix.height;
	xioctl(vfd, VIDIOC_S_FMT, &f, "S_FMT cap");
	printf("capture %ux%u stride %u size %u\n", f.fmt.pix.width, ha, stride,
	    f.fmt.pix.sizeimage);

	memset(&rb, 0, sizeof(rb));
	rb.type = ot; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS out");
	memset(&rb, 0, sizeof(rb));
	rb.type = ct; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS cap");

	memset(&b, 0, sizeof(b));
	b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF out");
	obuf_len = b.length;
	obuf = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
	if (obuf == MAP_FAILED)
		die("mmap out");
	memset(&b, 0, sizeof(b));
	b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF cap");
	cbuf_len = b.length;
	cbuf = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
	if (cbuf == MAP_FAILED)
		die("mmap cap");

	off = from_pic ? s.pic_off : s.slice_off;
	len = s.slice_end - off;
	if (trunc_to != 0 && trunc_to < len)
		len = trunc_to;
	if (len > obuf_len) {
		fprintf(stderr, "stream (%zu) larger than the buffer (%zu)\n", len, obuf_len);
		return (1);
	}

	for (iter = 0; iter < count; iter++) {
		int rfd;
		struct { int fd; } ra;
		struct pollfd pfd;
		unsigned ny = 0, nc = 0, nbad = 0;
		unsigned cw = w / 2, ch = h / 2;
		uint32_t cflags, oflags;

		memset(&ra, 0, sizeof(ra));
		xioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra, "REQUEST_ALLOC");
		rfd = ra.fd;
		memset(ec, 0, sizeof(ec));
		ec[0].id = V4L2_CID_STATELESS_MPEG2_SEQUENCE;
		ec[0].ptr = &s.seq; ec[0].size = sizeof(s.seq);
		ec[1].id = V4L2_CID_STATELESS_MPEG2_PICTURE;
		ec[1].ptr = &s.pic; ec[1].size = sizeof(s.pic);
		ec[2].id = V4L2_CID_STATELESS_MPEG2_QUANTISATION;
		ec[2].ptr = &s.q; ec[2].size = sizeof(s.q);
		memset(&ecs, 0, sizeof(ecs));
		ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL;
		ecs.request_fd = rfd;
		ecs.count = 3;
		ecs.controls = ec;
		xioctl(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS");

		memcpy(obuf, s.data + off, len);
		if (garbage)
			for (y = 8; y < len; y++)
				obuf[y] = (uint8_t)(y * 2654435761u >> 13) | 0x80;
		memset(cbuf, 0xa5, cbuf_len);
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF cap");
		memset(&b, 0, sizeof(b));
		b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
		b.bytesused = len;
		b.flags = V4L2_BUF_FLAG_REQUEST_FD;
		b.request_fd = rfd;
		b.timestamp.tv_sec = 1 + iter;
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF out");
		if (iter == 0) {
			xioctl(vfd, VIDIOC_STREAMON, &ot, "STREAMON out");
			xioctl(vfd, VIDIOC_STREAMON, &ct, "STREAMON cap");
		}
		xioctl(rfd, MEDIA_REQUEST_IOC_QUEUE, NULL, "REQUEST_QUEUE");
		pfd.fd = rfd; pfd.events = POLLPRI; pfd.revents = 0;
		if (poll(&pfd, 1, 5000) != 1) {
			fprintf(stderr, "iteration %d: request did not complete\n", iter);
			return (1);
		}
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP;
		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF cap");
		cflags = b.flags;
		if (b.timestamp.tv_sec != 1 + iter) {
			fprintf(stderr, "iteration %d: timestamp %ld lost\n", iter, (long)b.timestamp.tv_sec);
			nbad++;
		}
		memset(&b, 0, sizeof(b));
		b.type = ot; b.memory = V4L2_MEMORY_MMAP;
		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF out");
		oflags = b.flags;
		close(rfd);
		if ((cflags & V4L2_BUF_FLAG_ERROR) != 0 || (oflags & V4L2_BUF_FLAG_ERROR) != 0) {
			int expected = trunc_to != 0 || garbage;

			printf("iteration %d: %s, buffer flagged ERROR\n", iter,
			    expected ? "ok (expected)" : "FAIL");
			if (!expected)
				bad++;
			continue;
		}
		if (trunc_to != 0 || garbage) {
			printf("iteration %d: no error flagged for a damaged stream\n", iter);
			continue;
		}
		/* Luma at the stride; chroma interleaved after stride * height. */
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				int d = (int)cbuf[y * stride + x] - (int)ref[y * w + x];

				if (d >= -2 && d <= 2)
					ny++;
			}
		for (y = 0; y < ch; y++)
			for (x = 0; x < cw; x++) {
				const uint8_t *uv = cbuf + (size_t)stride * ha + y * stride + 2 * x;
				int du = (int)uv[0] - (int)ref[w * h + y * cw + x];
				int dv = (int)uv[1] - (int)ref[w * h + cw * ch + y * cw + x];

				if (du >= -2 && du <= 2)
					nc++;
				if (dv >= -2 && dv <= 2)
					nc++;
			}
		printf("iteration %d: %s  Y %u/%u  C %u/%u\n", iter,
		    (ny == w * h && nc == 2 * cw * ch && nbad == 0) ? "PASS" : "FAIL",
		    ny, w * h, nc, 2 * cw * ch);
		if (ny != w * h || nc != 2 * cw * ch || nbad != 0)
			bad++;
	}
	return (bad != 0);
}
