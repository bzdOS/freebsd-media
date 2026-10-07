/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_h264: decode an H.264 Annex B stream through the stateless V4L2 interface
 * and compare every picture with a reference decode (raw yuv420p in display order, e.g.
 * ffmpeg's). H.264 decoding is bit exact, so the comparison is too.
 *
 *   cedrus_h264 [-n pictures] [-v video] [-m media] [-t bytes] [-k] stream.264 reference.yuv
 *
 * The slices of a picture are queued one request each, the OUTPUT buffers but the last
 * flagged HOLD_CAPTURE_BUF; the capture buffers that still hold reference pictures are
 * kept out of the pool as a real decoder does. -t N cuts every slice to N bytes (a
 * damaged stream: the buffers must come back flagged ERROR and the decoder must go on);
 * -k keeps feeding the slices of a picture after one failed (every request must still end).
 *
 *   cc -I<shim> -I<dir with linux/videodev2.h> -o cedrus_h264 cedrus_h264.c
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

#include "h264ctl.h"

#define	NCAP	20

static int vfd, mfd;
static uint8_t *obuf, *cbuf[NCAP];
static size_t obuf_len, cbuf_len;
static struct v4l2_ctrl_h264_scaling_matrix flat_sm;
static struct v4l2_ctrl_h264_pred_weights no_pw;

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

/* Decode one slice into capture buffer `cap`; returns the buffer flags. */
static uint32_t
decode_slice(struct h264_pic *pic, unsigned s, unsigned cap, unsigned pidx, int first,
    size_t trunc_to)
{
	struct v4l2_ext_control ec[6];
	struct v4l2_ext_controls ecs;
	struct v4l2_buffer b;
	struct { int fd; } ra;
	struct pollfd pfd;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int last = (s + 1 == pic->nslices), rfd;
	unsigned nc = 0;
	uint32_t flags = 0, len = pic->slice[s].len;

	if (trunc_to != 0 && trunc_to < len)
		len = trunc_to;
	if (len > obuf_len) {
		fprintf(stderr, "slice of %u bytes larger than the buffer\n", len);
		exit(1);
	}
	memset(&ra, 0, sizeof(ra));
	xioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra, "REQUEST_ALLOC");
	rfd = ra.fd;
	memset(ec, 0, sizeof(ec));
	ec[nc].id = V4L2_CID_STATELESS_H264_SPS; ec[nc].ptr = &pic->sps; ec[nc].size = sizeof(pic->sps); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_PPS; ec[nc].ptr = &pic->pps; ec[nc].size = sizeof(pic->pps); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
	ec[nc].ptr = pic->has_sm ? (void *)&pic->sm : (void *)&flat_sm; ec[nc].size = sizeof(flat_sm); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS;
	ec[nc].ptr = pic->slice[s].has_pw ? (void *)&pic->slice[s].pw : (void *)&no_pw;
	ec[nc].size = sizeof(no_pw); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_SLICE_PARAMS;
	ec[nc].ptr = &pic->slice[s].sp; ec[nc].size = sizeof(pic->slice[s].sp); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
	ec[nc].ptr = &pic->dp; ec[nc].size = sizeof(pic->dp); nc++;
	memset(&ecs, 0, sizeof(ecs));
	ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL;
	ecs.request_fd = rfd;
	ecs.count = nc;
	ecs.controls = ec;
	xioctl(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS");

	memcpy(obuf, pic->slice[s].data, len);
	if (s == 0) {
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = cap;
		xioctl(vfd, VIDIOC_QBUF, &b, "QBUF cap");
	}
	memset(&b, 0, sizeof(b));
	b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	b.bytesused = len;
	b.flags = V4L2_BUF_FLAG_REQUEST_FD | (last ? 0 : V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF);
	b.request_fd = rfd;
	b.timestamp.tv_sec = (pidx + 1) / 1000000;
	b.timestamp.tv_usec = (pidx + 1) % 1000000;
	xioctl(vfd, VIDIOC_QBUF, &b, "QBUF out");
	if (first) {
		xioctl(vfd, VIDIOC_STREAMON, &ot, "STREAMON out");
		xioctl(vfd, VIDIOC_STREAMON, &ct, "STREAMON cap");
	}
	xioctl(rfd, MEDIA_REQUEST_IOC_QUEUE, NULL, "REQUEST_QUEUE");
	pfd.fd = rfd; pfd.events = POLLPRI; pfd.revents = 0;
	if (poll(&pfd, 1, 5000) != 1) {
		fprintf(stderr, "picture %u slice %u: request did not complete\n", pidx, s);
		exit(1);
	}
	if (last) {
		memset(&b, 0, sizeof(b));
		b.type = ct; b.memory = V4L2_MEMORY_MMAP;
		xioctl(vfd, VIDIOC_DQBUF, &b, "DQBUF cap");
		flags = b.flags;
		if (b.index != cap || (unsigned long)b.timestamp.tv_sec * 1000000 +
		    (unsigned long)b.timestamp.tv_usec != pidx + 1) {
			fprintf(stderr, "picture %u: got buffer %u stamp %ld\n", pidx, b.index,
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

int
main(int argc, char **argv)
{
	const char *vpath = "/dev/video0", *mpath = "/dev/media0";
	int c, maxpics = 1 << 30, bad = 0, first = 1, r, keep = 0;
	size_t trunc_to = 0, len, pos = 0, rlen;
	uint8_t *data, *ref;
	struct h264_ctx *hc = calloc(1, sizeof(*hc)), *hc2 = calloc(1, sizeof(*hc));
	struct h264_pic *pic = calloc(1, sizeof(*pic));
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	unsigned w, h, stride, ha, k, ncap, npics = 0, cap;
	int disp[4096], idr[4096], poc[4096];
	uint64_t cap_ts[NCAP];
	FILE *fp;

	while ((c = getopt(argc, argv, "n:v:m:t:k")) != -1) {
		switch (c) {
		case 'n': maxpics = atoi(optarg); break;
		case 'v': vpath = optarg; break;
		case 'm': mpath = optarg; break;
		case 't': trunc_to = atoi(optarg); break;
		case 'k': keep = 1; break;
		default: return (2);
		}
	}
	if (argc - optind != 2) {
		fprintf(stderr, "usage: cedrus_h264 [-n pictures] [-v video] [-m media] [-t bytes] [-k] "
		    "stream.264 ref.yuv\n");
		return (2);
	}
	memset(&flat_sm, 16, sizeof(flat_sm));
	fp = fopen(argv[optind], "rb");
	if (fp == NULL)
		die(argv[optind]);
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc(len + 16);
	if (fread(data, 1, len, fp) != len)
		die("stream");
	fclose(fp);
	fp = fopen(argv[optind + 1], "rb");
	if (fp == NULL)
		die(argv[optind + 1]);
	fseek(fp, 0, SEEK_END);
	rlen = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	ref = malloc(rlen);
	if (fread(ref, 1, rlen, fp) != rlen)
		die("reference");
	fclose(fp);

	/* First pass: sizes and the display order (by picture order count per IDR period). */
	{
		unsigned base = 0, g0 = 0, i, j;

		pos = 0;
		hc2->strict = 1;
		while (npics < 4096 && (r = h264_next_picture(hc2, data, len, &pos, pic, 1000)) == 1) {
			if (npics == 0) {
				w = (pic->sps.pic_width_in_mbs_minus1 + 1) * 16;
				h = (pic->sps.pic_height_in_map_units_minus1 + 1) * 16;
			}
			idr[npics] = pic->idr || pic->mmco5;
			poc[npics] = pic->poc;
			npics++;
		}
		if (npics == 0) {
			fprintf(stderr, "cannot parse the stream (%d)\n", r);
			return (1);
		}
		for (i = 0; i <= npics; i++) {
			if (i == npics || (idr[i] && i > g0)) {
				for (j = g0; j < i; j++) {
					unsigned rank = 0, m;

					for (m = g0; m < i; m++)
						if (poc[m] < poc[j])
							rank++;
					disp[j] = base + rank;
				}
				base += i - g0;
				g0 = i;
			}
		}
	}
	if (npics > (unsigned)maxpics)
		npics = maxpics;

	vfd = open(vpath, O_RDWR);
	mfd = open(mpath, O_RDWR);
	if (vfd < 0 || mfd < 0)
		die("open");
	memset(&f, 0, sizeof(f));
	f.type = ot;
	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE;
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
	{
		struct v4l2_ext_control ec[2];
		struct v4l2_ext_controls ecs;

		memset(ec, 0, sizeof(ec));
		memset(&ecs, 0, sizeof(ecs));
		ec[0].id = V4L2_CID_STATELESS_H264_DECODE_MODE;
		ec[0].value = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED;
		ec[1].id = V4L2_CID_STATELESS_H264_START_CODE;
		ec[1].value = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
		ecs.which = V4L2_CTRL_WHICH_CUR_VAL;
		ecs.count = 2;
		ecs.controls = ec;
		xioctl(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS decode mode");
	}
	printf("%ux%u, capture stride %u height %u, %u pictures\n", w, h, stride, ha, npics);

	memset(&rb, 0, sizeof(rb));
	rb.type = ot; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS out");
	memset(&rb, 0, sizeof(rb));
	/* Large pictures need large contiguous buffers: ask for fewer when memory is short. */
	for (k = NCAP; ; k /= 2) {
		memset(&rb, 0, sizeof(rb));
		rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.count = k; rb.memory = V4L2_MEMORY_MMAP;
		if (ioctl(vfd, VIDIOC_REQBUFS, &rb) == 0)
			break;
		if (k <= 6)
			die("REQBUFS cap");
	}
	ncap = rb.count > NCAP ? NCAP : rb.count;
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
		cap_ts[k] = 0;
	}

	/* Second pass: decode. */
	memset(hc, 0, sizeof(*hc));
	hc->strict = 1;
	pos = 0;
	for (k = 0; k < npics; k++) {
		unsigned s, i, ny = 0, nc = 0, x, y, cw = w / 2, ch = h / 2;
		uint32_t flags = 0;
		uint64_t ts = (uint64_t)(k + 1) * 1000;
		const uint8_t *rp;
		int expected = trunc_to != 0, ok;

		r = h264_next_picture(hc, data, len, &pos, pic, ts);
		if (r != 1) {
			fprintf(stderr, "picture %u: parse failed (%d)\n", k, r);
			return (1);
		}
		/* a capture buffer whose picture is no longer a reference (the DPB is already past this picture) */
		for (cap = 0; cap < ncap; cap++) {
			int used = 0;

			for (i = 0; i < 16; i++)
				if (hc->dpb[i].used && hc->dpb[i].ts == cap_ts[cap])
					used = 1;
			/*
			 * The DPB above is already past this picture (its sliding window has
			 * dropped the oldest reference), but this picture still names that
			 * reference in its own decode parameters: decoding into its buffer
			 * would overwrite it while it is being read.
			 */
			for (i = 0; i < 16; i++)
				if ((pic->dp.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) != 0 &&
				    pic->dp.dpb[i].reference_ts == cap_ts[cap] &&
				    cap_ts[cap] != 0)
					used = 1;
			if (!used)
				break;
		}
		if (cap == ncap) {
			fprintf(stderr, "no free capture buffer\n");
			return (1);
		}
		cap_ts[cap] = ts;
		for (s = 0; s < pic->nslices; s++) {
			uint32_t fl = decode_slice(pic, s, cap, k, first, trunc_to);

			first = 0;
			flags |= fl;
			/*
			 * A slice that comes back with ERROR has already had this picture's
			 * capture buffer completed by the core, so queueing the remaining
			 * slices would leave their requests with no destination to run on —
			 * and they would never complete.  An application stops feeding a
			 * picture whose slice failed; the test does the same instead of
			 * waiting for a request that cannot finish.
			 */
			if ((fl & V4L2_BUF_FLAG_ERROR) != 0 && !keep)
				break;
		}
		if ((flags & V4L2_BUF_FLAG_ERROR) != 0) {
			printf("picture %u: %s, buffer flagged ERROR\n", k, expected ? "ok (expected)" : "FAIL");
			if (!expected)
				bad++;
			continue;
		}
		if (expected) {
			printf("picture %u: no error flagged for a damaged stream\n", k);
			continue;
		}
		if ((size_t)(disp[k] + 1) * (w * h * 3 / 2) > rlen) {
			printf("picture %u: no reference frame %d\n", k, disp[k]);
			continue;
		}
		rp = ref + (size_t)disp[k] * (w * h * 3 / 2);
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++)
				ny += cbuf[cap][y * stride + x] == rp[y * w + x];
		for (y = 0; y < ch; y++)
			for (x = 0; x < cw; x++) {
				const uint8_t *uv = cbuf[cap] + (size_t)stride * ha + y * stride + 2 * x;

				nc += uv[0] == rp[w * h + y * cw + x];
				nc += uv[1] == rp[w * h + cw * ch + y * cw + x];
			}
		ok = (ny == w * h && nc == 2 * cw * ch);
		printf("picture %u (%c, display %d, %u slice%s): %s  Y %u/%u  C %u/%u\n", k,
		    "PBI"[pic->slice[0].sp.slice_type < 3 ? pic->slice[0].sp.slice_type : 0], disp[k],
		    pic->nslices, pic->nslices == 1 ? "" : "s", ok ? "PASS" : "FAIL", ny, w * h, nc,
		    2 * cw * ch);
		if (!ok) {
			/*
			 * Verification, not part of the test: is the buffer's content wrong,
			 * or was the first read stale?  Read it again at once and again after
			 * the engine has certainly stopped.
			 */
			unsigned long ny2 = 0, nc2 = 0;

			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					ny2 += cbuf[cap][y * stride + x] == rp[y * w + x];
			printf("    re-read at once   : Y %lu/%u%s\n", ny2, w * h,
			    ny2 == ny ? "  (same)" : "  (DIFFERENT: the first read was stale)");
			usleep(50000);
			ny2 = nc2 = 0;
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					ny2 += cbuf[cap][y * stride + x] == rp[y * w + x];
			for (y = 0; y < ch; y++)
				for (x = 0; x < cw; x++) {
					const uint8_t *uv = cbuf[cap] + (size_t)stride * ha + y * stride + 2 * x;

					nc2 += uv[0] == rp[w * h + y * cw + x];
					nc2 += uv[1] == rp[w * h + cw * ch + y * cw + x];
				}
			printf("    re-read after 50ms: Y %lu/%u C %lu/%u%s\n", ny2, w * h, nc2, 2 * cw * ch,
			    (ny2 == ny && nc2 == nc) ? "  (same: the buffer really holds this)"
			    : "  (DIFFERENT: the first read was stale)");
			/* which macroblocks differ ('.' none, 'X' luma differs) */
			for (y = 0; y < h / 16; y++) {
				printf("    mb row %2u: ", y);
				for (x = 0; x < w / 16; x++) {
					unsigned i2, j2, d = 0;

					for (j2 = 0; j2 < 16; j2++)
						for (i2 = 0; i2 < 16; i2++)
							d += cbuf[cap][(y * 16 + j2) * stride + x * 16 + i2] !=
							    rp[(y * 16 + j2) * w + x * 16 + i2];
					putchar(d == 0 ? '.' : 'X');
				}
				putchar('\n');
			}
			bad++;
		}
	}
	return (bad != 0);
}
