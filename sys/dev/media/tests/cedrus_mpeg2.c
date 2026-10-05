/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_mpeg2: decode an MPEG-2 elementary stream through the stateless V4L2
 * interface (/dev/video*, /dev/media*) and compare every picture with a
 * reference decode (raw yuv420p in display order, e.g. from ffmpeg).
 *
 *   cedrus_mpeg2 [-a] [-p] [-n count] [-t bytes] [-g] [-v video] [-m media]
 *                stream.m2v reference.yuv
 *
 * Without -a only the first picture is decoded (`count` times, default 2).
 * -a   decode every picture in stream order, I, P and B, keeping the
 *      reference pictures in their capture buffers and naming them in the
 *      requests of the pictures that use them, as a real decoder does
 * -p   give the driver the data from the picture start code (headers and all)
 *      instead of from the first slice start code
 * -t N feed only the first N bytes of the slice data (a damaged stream: the
 *      buffer must come back flagged ERROR, and the next picture must decode)
 * -g   feed garbage instead of slice data (after the first start code)
 *
 * The headers are parsed here the way a userland decoder would, and handed to
 * the driver as the three MPEG-2 controls inside a media request.
 *
 *   cc -I<shim> -I<dir with linux/videodev2.h> -o cedrus_mpeg2 cedrus_mpeg2.c
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

#define	MAXPIC	256
#define	NCAP	8

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

static void
xioctl(int fd, unsigned long req, void *arg, const char *what)
{

	if (ioctl(fd, req, arg) != 0) {
		fprintf(stderr, "%s: ", what);
		die("ioctl");
	}
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

struct picture {
	size_t		pic_off;	/* picture start code (00 00 01 00) */
	size_t		slice_off;	/* first slice start code */
	size_t		end;		/* end of this picture's slices */
	unsigned	tref;		/* temporal reference within its GOP */
	unsigned	display;	/* position in the whole stream, display order */
	int		second;		/* the second field of a frame (same buffer as the first) */
	struct v4l2_ctrl_mpeg2_picture pic;
};

struct stream {
	uint8_t		*data;
	size_t		len;
	struct picture	pics[MAXPIC];
	unsigned	npics;
	struct v4l2_ctrl_mpeg2_sequence seq;
	struct v4l2_ctrl_mpeg2_quantisation q;
};

static void
parse(struct stream *s, const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t i, n;
	unsigned gop_base = 0, gop_count = 0;
	struct picture *cur = NULL;

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
	memset(&s->q, 0, sizeof(s->q));
	memcpy(s->q.intra_quantiser_matrix, default_intra, 64);
	memset(s->q.non_intra_quantiser_matrix, 16, 64);
	s->npics = 0;
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

			if (cur != NULL && cur->slice_off != 0 && cur->end == 0)
				cur->end = i;
			if (s->seq.horizontal_size != 0)
				continue;	/* repeated sequence header */
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
		} else if (c == 0xb8) {
			if (cur != NULL && cur->slice_off != 0 && cur->end == 0)
				cur->end = i;
			gop_base += gop_count;
			gop_count = 0;
		} else if (c == 0x00) {
			if (cur != NULL && cur->slice_off != 0 && cur->end == 0)
				cur->end = i;
			if (s->npics == MAXPIC)
				break;
			cur = &s->pics[s->npics++];
			memset(cur, 0, sizeof(*cur));
			cur->pic_off = i;
			cur->tref = getbits(s->data, p, 10);
			cur->display = gop_base + cur->tref;
			cur->pic.picture_coding_type = getbits(s->data, p + 10, 3);
			gop_count++;
		} else if (c == 0xb5 && cur != NULL && getbits(s->data, p, 4) == 8) {
			size_t q = p + 4;
			unsigned fl = 0;

			cur->pic.f_code[0][0] = getbits(s->data, q, 4);
			cur->pic.f_code[0][1] = getbits(s->data, q + 4, 4);
			cur->pic.f_code[1][0] = getbits(s->data, q + 8, 4);
			cur->pic.f_code[1][1] = getbits(s->data, q + 12, 4);
			q += 16;
			cur->pic.intra_dc_precision = getbits(s->data, q, 2);
			cur->pic.picture_structure = getbits(s->data, q + 2, 2);
			if (getbits(s->data, q + 4, 1)) fl |= V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST;
			if (getbits(s->data, q + 5, 1)) fl |= V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT;
			if (getbits(s->data, q + 6, 1)) fl |= V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV;
			if (getbits(s->data, q + 7, 1)) fl |= V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE;
			if (getbits(s->data, q + 8, 1)) fl |= V4L2_MPEG2_PIC_FLAG_INTRA_VLC;
			if (getbits(s->data, q + 9, 1)) fl |= V4L2_MPEG2_PIC_FLAG_ALT_SCAN;
			if (getbits(s->data, q + 10, 1)) fl |= V4L2_MPEG2_PIC_FLAG_REPEAT_FIRST;
			if (getbits(s->data, q + 12, 1)) fl |= V4L2_MPEG2_PIC_FLAG_PROGRESSIVE;
			cur->pic.flags = fl;
			/* two field pictures with one temporal reference make one frame */
			if (cur->pic.picture_structure != V4L2_MPEG2_PIC_FRAME && s->npics >= 2) {
				struct picture *prev = cur - 1;

				if (prev->tref == cur->tref && prev->pic.picture_structure != V4L2_MPEG2_PIC_FRAME &&
				    prev->pic.picture_structure != cur->pic.picture_structure && !prev->second) {
					cur->second = 1;
					gop_count--;
				}
			}
		} else if (c >= 0x01 && c <= 0xaf && cur != NULL) {
			if (cur->slice_off == 0)
				cur->slice_off = i;
		} else if (c == 0xb7 && cur != NULL && cur->slice_off != 0 && cur->end == 0) {
			cur->end = i;
		}
	}
	if (cur != NULL && cur->slice_off != 0 && cur->end == 0)
		cur->end = s->len;
	if (s->npics == 0 || s->seq.horizontal_size == 0 || s->pics[0].slice_off == 0) {
		fprintf(stderr, "%s: not an MPEG-2 stream I can read\n", path);
		exit(1);
	}
}

static int vfd, mfd;
static uint8_t *obuf, *cbuf[NCAP];
static size_t obuf_len, cbuf_len;

/* The timestamp that names coded picture `i`, as the controls carry it (ns). */
static unsigned long ts_off;		/* makes the stamps of repeated passes unique */

static uint64_t
ts_ns(unsigned i)
{

	return ((uint64_t)(ts_off + i + 1) * 1000);
}

/*
 * Decode one picture into capture buffer `cap`. Returns the buffer flags of
 * the result (CAPTURE | OUTPUT).
 */
static uint32_t
decode(struct stream *s, unsigned idx, unsigned cap, uint64_t fwd, uint64_t back,
    int from_pic, size_t trunc_to, int garbage, int first, int hold, int newcap, int lastfield)
{
	struct picture *pc = &s->pics[idx];
	struct v4l2_ctrl_mpeg2_picture pic = pc->pic;
	struct v4l2_ext_control ec[3];
	struct v4l2_ext_controls ecs;
	struct v4l2_buffer b;
	struct { int fd; } ra;
	struct pollfd pfd;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	size_t off = from_pic ? pc->pic_off : pc->slice_off, len = pc->end - off, y;
	uint32_t flags;
	int rfd;

	if (trunc_to != 0 && trunc_to < len)
		len = trunc_to;
	if (len > obuf_len) {
		fprintf(stderr, "picture %u (%zu bytes) larger than the buffer\n", idx, len);
		exit(1);
	}
	pic.forward_ref_ts = fwd;
	pic.backward_ref_ts = back;
	memset(&ra, 0, sizeof(ra));
	xioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra, "REQUEST_ALLOC");
	rfd = ra.fd;
	memset(ec, 0, sizeof(ec));
	ec[0].id = V4L2_CID_STATELESS_MPEG2_SEQUENCE;
	ec[0].ptr = &s->seq; ec[0].size = sizeof(s->seq);
	ec[1].id = V4L2_CID_STATELESS_MPEG2_PICTURE;
	ec[1].ptr = &pic; ec[1].size = sizeof(pic);
	ec[2].id = V4L2_CID_STATELESS_MPEG2_QUANTISATION;
	ec[2].ptr = &s->q; ec[2].size = sizeof(s->q);
	memset(&ecs, 0, sizeof(ecs));
	ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL;
	ecs.request_fd = rfd;
	ecs.count = 3;
	ecs.controls = ec;
	xioctl(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS");

	memcpy(obuf, s->data + off, len);
	if (garbage)
		for (y = 8; y < len; y++)
			obuf[y] = (uint8_t)(y * 2654435761u >> 13) | 0x80;
	if (newcap) {
		memset(cbuf[cap], 0xa5, cbuf_len);
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = cap;
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF cap");
	}
	memset(&b, 0, sizeof(b));
	b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	b.bytesused = len;
	b.flags = V4L2_BUF_FLAG_REQUEST_FD | (hold ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0);
	b.request_fd = rfd;
	b.timestamp.tv_sec = (ts_off + idx + 1) / 1000000;
	b.timestamp.tv_usec = (ts_off + idx + 1) % 1000000;
	xioctl(vfd, VIDIOC_QBUF, &b, "QBUF out");
	if (first) {
		xioctl(vfd, VIDIOC_STREAMON, &ot, "STREAMON out");
		xioctl(vfd, VIDIOC_STREAMON, &ct, "STREAMON cap");
	}
	xioctl(rfd, MEDIA_REQUEST_IOC_QUEUE, NULL, "REQUEST_QUEUE");
	pfd.fd = rfd; pfd.events = POLLPRI; pfd.revents = 0;
	if (poll(&pfd, 1, 5000) != 1) {
		fprintf(stderr, "picture %u: request did not complete\n", idx);
		exit(1);
	}
	flags = 0;
	if (lastfield) {		/* the capture buffer is done with the last field */
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP;
		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF cap");
		flags = b.flags;
		if (b.index != cap || (unsigned long)b.timestamp.tv_sec * 1000000 +
		    (unsigned long)b.timestamp.tv_usec != ts_off + idx + 1) {
			fprintf(stderr, "picture %u: got buffer %u stamp %ld\n", idx, b.index,
			    (long)b.timestamp.tv_usec);
			flags |= V4L2_BUF_FLAG_ERROR;
		}
	}
	memset(&b, 0, sizeof(b));
	b.type = ot; b.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF out");
	close(rfd);
	return (flags | (b.flags & V4L2_BUF_FLAG_ERROR));
}

#ifdef CEDRUS_KMS
/* cedrus_kms.c includes this file: called with every decoded frame; nonzero stops. */
int kms_frame(int vfd, unsigned cap, unsigned stride, unsigned ha, unsigned w, unsigned h);
#endif

#ifdef CEDRUS_GPU
/* cedrus_gpu.c includes this file and takes over after the picture is decoded. */
int gpu_stage(int vfd, unsigned stride, unsigned ha, unsigned w, unsigned h,
    const uint8_t *nv12);
#endif

int
main(int argc, char **argv)
{
	const char *vpath = "/dev/video0", *mpath = "/dev/media0";
	int c, count = 0, from_pic = 0, all = 0, iter, bad = 0, garbage = 0;
	size_t trunc_to = 0;
	struct stream *s;
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	uint8_t *ref;
	size_t rlen;
	unsigned w, h, stride, ha, y, x, cw, ch, k, ncap;
	FILE *rf;
	int refcap[2] = { -1, -1 };	/* capture buffers holding the last two reference pictures */
	unsigned refidx[2] = { 0, 0 };
	int busy[NCAP];
	unsigned prevcap = 0;

	while ((c = getopt(argc, argv, "apn:v:m:t:g")) != -1) {
		switch (c) {
		case 'a': all = 1; break;
		case 'p': from_pic = 1; break;
		case 'n': count = atoi(optarg); break;
		case 'v': vpath = optarg; break;
		case 'm': mpath = optarg; break;
		case 't': trunc_to = atoi(optarg); break;
		case 'g': garbage = 1; break;
		default: return (2);
		}
	}
#ifdef CEDRUS_KMS
	count = 1000000;		/* play until kms_frame() says stop */
	all = 1;
#endif
	if (argc - optind != 2) {
		fprintf(stderr, "usage: cedrus_mpeg2 [-a] [-p] [-n count] [-t bytes] [-g] "
		    "[-v video] [-m media] stream.m2v ref.yuv\n");
		return (2);
	}
	s = calloc(1, sizeof(*s));
	parse(s, argv[optind]);
	w = s->seq.horizontal_size;
	h = s->seq.vertical_size;
	cw = w / 2;
	ch = h / 2;
	rf = fopen(argv[optind + 1], "rb");
	if (rf == NULL)
		die(argv[optind + 1]);
	fseek(rf, 0, SEEK_END);
	rlen = ftell(rf);
	fseek(rf, 0, SEEK_SET);
	ref = malloc(rlen);
	if (fread(ref, 1, rlen, rf) != rlen)
		die("reference");
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
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(vfd, VIDIOC_G_FMT, &f, "G_FMT cap");
	if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
		fprintf(stderr, "capture format is not NV12\n");
		return (1);
	}
	stride = f.fmt.pix.bytesperline;
	ha = f.fmt.pix.height;
	xioctl(vfd, VIDIOC_S_FMT, &f, "S_FMT cap");
	printf("capture %ux%u stride %u size %u, %u pictures in the stream\n",
	    f.fmt.pix.width, ha, stride, f.fmt.pix.sizeimage, s->npics);

	memset(&rb, 0, sizeof(rb));
	rb.type = ot; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS out");
	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.count = all ? NCAP : 1;
	rb.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS cap");
	ncap = rb.count;
	if (ncap > NCAP)
		ncap = NCAP;

	memset(&b, 0, sizeof(b));
	b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF out");
	obuf_len = b.length;
	obuf = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
	if (obuf == MAP_FAILED)
		die("mmap out");
	for (k = 0; k < ncap; k++) {
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = k;
		xioctl(vfd, VIDIOC_QUERYBUF, &b, "QUERYBUF cap");
		cbuf_len = b.length;
		cbuf[k] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
		if (cbuf[k] == MAP_FAILED)
			die("mmap cap");
		busy[k] = 0;
	}

	for (iter = 0; iter < (count ? count : (all ? 1 : 2)); iter++) {
		unsigned npic = all ? s->npics : 1, i;

		ts_off = (unsigned long)iter * 1000;

		refcap[0] = refcap[1] = -1;
		memset(busy, 0, sizeof(busy));
		for (i = 0; i < npic; i++) {
			struct picture *pc = &s->pics[i];
			unsigned type = pc->pic.picture_coding_type;
			uint64_t fwd = 0, back = 0;
			unsigned ny = 0, nc = 0, cap;
			const uint8_t *rp;
			uint32_t flags;
			int expected = trunc_to != 0 || garbage;
			int ok;

			/* The references: the last reference is the backward one of a B. */
			if (type == V4L2_MPEG2_PIC_CODING_TYPE_P && refcap[0] >= 0)
				fwd = back = ts_ns(refidx[0]);
			else if (type == V4L2_MPEG2_PIC_CODING_TYPE_B && refcap[0] >= 0) {
				back = ts_ns(refidx[0]);
				fwd = refcap[1] >= 0 ? ts_ns(refidx[1]) : back;
			}
			if (pc->second)
				cap = prevcap;		/* the second field goes into the first one's buffer */
			else {
				for (cap = 0; cap < ncap && busy[cap]; cap++)
					;
				if (cap == ncap) {
					fprintf(stderr, "no free capture buffer\n");
					return (1);
				}
			}
			{
				int field = pc->pic.picture_structure != V4L2_MPEG2_PIC_FRAME;
				int hold = field && !pc->second;
				int lastfield = !hold;

				flags = decode(s, i, cap, fwd, back, from_pic, trunc_to, garbage,
				    iter == 0 && i == 0, hold, !pc->second, lastfield);
				prevcap = cap;
				if (hold) {
					busy[cap] = 1;	/* waiting for its second field */
					continue;
				}
			}
			if (type == V4L2_MPEG2_PIC_CODING_TYPE_B)
				busy[cap] = 0;		/* never a reference */
#ifdef CEDRUS_KMS
			if ((flags & V4L2_BUF_FLAG_ERROR) == 0 &&
			    kms_frame(vfd, cap, stride, ha, w, h))
				return (0);
#endif
			if (type != V4L2_MPEG2_PIC_CODING_TYPE_B) {
				/* A new reference: the one before the previous goes free. */
				if (refcap[1] >= 0)
					busy[refcap[1]] = 0;
				refcap[1] = refcap[0];
				refidx[1] = refidx[0];
				refcap[0] = cap;
				refidx[0] = i;
				busy[cap] = 1;
			}
			if ((flags & V4L2_BUF_FLAG_ERROR) != 0) {
				printf("picture %u (%c): %s, buffer flagged ERROR\n", i,
				    "?IPBD"[type < 5 ? type : 0], expected ? "ok (expected)" : "FAIL");
				if (!expected)
					bad++;
				continue;
			}
			if (expected) {
				printf("picture %u: no error flagged for a damaged stream\n", i);
				continue;
			}
#ifdef CEDRUS_KMS
			continue;		/* playing, not checking */
#endif
			if ((size_t)(pc->display + 1) * (w * h * 3 / 2) > rlen) {
				printf("picture %u: no reference frame %u\n", i, pc->display);
				continue;
			}
			rp = ref + (size_t)pc->display * (w * h * 3 / 2);
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++) {
					int d = (int)cbuf[cap][y * stride + x] - (int)rp[y * w + x];

					if (d >= -2 && d <= 2)
						ny++;
				}
			for (y = 0; y < ch; y++)
				for (x = 0; x < cw; x++) {
					const uint8_t *uv = cbuf[cap] + (size_t)stride * ha + y * stride + 2 * x;
					int du = (int)uv[0] - (int)rp[w * h + y * cw + x];
					int dv = (int)uv[1] - (int)rp[w * h + cw * ch + y * cw + x];

					if (du >= -2 && du <= 2)
						nc++;
					if (dv >= -2 && dv <= 2)
						nc++;
				}
			ok = (ny == w * h && nc == 2 * cw * ch);
			printf("picture %u (%c, display %u): %s  Y %u/%u  C %u/%u\n", i,
			    "?IPBD"[type < 5 ? type : 0], pc->display, ok ? "PASS" : "FAIL",
			    ny, w * h, nc, 2 * cw * ch);
			if (!ok)
				bad++;
		}
	}
#ifdef CEDRUS_GPU
	if (bad == 0 && !all)
		bad = gpu_stage(vfd, stride, ha, w, h, cbuf[0]);
#endif
	return (bad != 0);
}
