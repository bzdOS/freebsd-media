/* SPDX-License-Identifier: BSD-2-Clause */
/* init_h264.c -- initramfs /init for the register-trace rig: decode an embedded H.264
 * stream through the kernel's V4L2 stateless interface on the A64 video engine, one
 * request per slice, print a hash of every decoded picture, then idle. No libc. */
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include "h264ctl.h"
#include "h264s.h"   /* defines h264_streams[], H264_NSTREAMS */

void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define S3(n,a,b,c) sc(n,(long)(a),(long)(b),(long)(c),0,0,0)
static int openf(const char *p, int fl) { return (int)sc(56, -100, (long)p, fl, 0, 0, 0); }
static long ioc(int fd, unsigned long r, void *a) { return S3(29, fd, r, a); }
static int kfd = 1;
static void out(const char *s) { long n = 0; while (s[n]) n++; S3(64, kfd, s, n); }
static void hex(unsigned long v)
{
	char b[20]; int i;
	b[0] = '0'; b[1] = 'x';
	for (i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	b[18] = ' '; b[19] = 0;
	out(b);
}
static void say(const char *s, long v) { out(s); hex((unsigned long)v); out("\n"); }
static void zero(void *p, long n) { char *c = p; while (n--) *c++ = 0; }

void __attribute__((noreturn)) idle(void)
{
	for (;;) { struct { long s, n; } ts = { 100, 0 }; S3(101, &ts, 0, 0); }
}
void __attribute__((noreturn)) fail(const char *what, long rc)
{
	say(what, rc);
	out("TRACE-DONE\n");
	idle();
}

static struct h264_ctx hctx;
static struct h264_pic pic;

#ifndef H264_MAX_PICS
#define H264_MAX_PICS 8
#endif
#ifndef H264_MAX_FRAMES
#define H264_MAX_FRAMES H264_MAX_PICS
#endif
static int vfd = -1, mfd = -1;
static void *obuf, *cbuf[H264_MAX_FRAMES];
static unsigned long obuf_len, cbuf_len, stride;
static int streaming, allocated;

static int setup(unsigned long w, unsigned long h)
{
	struct v4l2_format f;
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer b;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int k;

	if (allocated) {
		if (streaming) {
			ioc(vfd, VIDIOC_STREAMOFF, &ot);
			ioc(vfd, VIDIOC_STREAMOFF, &ct);
		}
		sc(215, (long)obuf, obuf_len, 0, 0, 0, 0); /* munmap */
		for (k = 0; k < H264_MAX_FRAMES; k++)
			sc(215, (long)cbuf[k], cbuf_len, 0, 0, 0, 0);
		zero(&rb, sizeof rb); rb.type = ot; rb.count = 0; rb.memory = V4L2_MEMORY_MMAP; ioc(vfd, VIDIOC_REQBUFS, &rb);
		zero(&rb, sizeof rb); rb.type = ct; rb.count = 0; rb.memory = V4L2_MEMORY_MMAP; ioc(vfd, VIDIOC_REQBUFS, &rb);
		streaming = 0;
		allocated = 0;
	}
	zero(&f, sizeof f);
	f.type = ot;
	f.fmt.pix.width = w; f.fmt.pix.height = h; f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	f.fmt.pix.sizeimage = 0x40000;
	say("s_fmt out", ioc(vfd, VIDIOC_S_FMT, &f));
	zero(&f, sizeof f); f.type = ct;
	ioc(vfd, VIDIOC_G_FMT, &f);
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	ioc(vfd, VIDIOC_S_FMT, &f);
	zero(&f, sizeof f); f.type = ct;
	ioc(vfd, VIDIOC_G_FMT, &f);
	say(" cap fourcc", f.fmt.pix.pixelformat); say(" cap stride", f.fmt.pix.bytesperline);
	stride = f.fmt.pix.bytesperline;
	zero(&rb, sizeof rb); rb.type = ot; rb.count = 1; rb.memory = V4L2_MEMORY_MMAP;
	ioc(vfd, VIDIOC_REQBUFS, &rb);
	zero(&rb, sizeof rb); rb.type = ct; rb.count = H264_MAX_FRAMES; rb.memory = V4L2_MEMORY_MMAP;
	ioc(vfd, VIDIOC_REQBUFS, &rb);
	say(" cap buffers", rb.count);
	if (rb.count < H264_MAX_FRAMES)
		fail("insufficient capture buffers", rb.count);
	zero(&b, sizeof b); b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
	ioc(vfd, VIDIOC_QUERYBUF, &b);
	obuf_len = b.length;
	obuf = (void *)sc(222, 0, b.length, 3, 1, vfd, b.m.offset);
	for (k = 0; k < H264_MAX_FRAMES && k < (int)rb.count; k++) {
		zero(&b, sizeof b); b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = k;
		ioc(vfd, VIDIOC_QUERYBUF, &b);
		cbuf[k] = (void *)sc(222, 0, b.length, 3, 1, vfd, b.m.offset);
		cbuf_len = b.length;
	}
	allocated = 1;
	return (0);
}

/* Experiment variants (see mkhdr.py): change one control value, leaving the
 * bitstream untouched, so a trace diff shows what that value becomes. */
#define VAR_NONE   0
#define VAR_CABAC2 1	/* cabac_init_idc = 2 */
#define VAR_BW     2	/* explicit B weights, both lists weight 1 (neutral) */
#define VAR_CABAC1 3	/* cabac_init_idc = 1: tells bits[1:0] from a single bit */
#define VAR_NO_LT  5	/* clear only the DPB long-term flags: trace their effect */
#define VAR_BW2    4	/* explicit B weights, list 0 weight 1, list 1 weight 3:
			 * the word values then show which half of SRAM 0 is list 1 */

static void apply_variant(struct h264_pic *p, int var)
{
	unsigned s, l, i, k;

	if (var == VAR_NO_LT) {
		for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++)
			p->dp.dpb[i].flags &= ~V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
	} else if (var == VAR_CABAC2 || var == VAR_CABAC1) {
		int idc = (var == VAR_CABAC2) ? 2 : 1;

		for (s = 0; s < p->nslices; s++) {
			unsigned st = p->slice[s].sp.slice_type;

			if ((p->pps.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) &&
			    st != V4L2_H264_SLICE_TYPE_I && st != V4L2_H264_SLICE_TYPE_SI)
				p->slice[s].sp.cabac_init_idc = idc;
		}
	} else if (var == VAR_BW || var == VAR_BW2) {
		p->pps.weighted_bipred_idc = 1;
		for (s = 0; s < p->nslices; s++) {
			struct h264_slice_info *si = &p->slice[s];

			if (si->sp.slice_type != V4L2_H264_SLICE_TYPE_B)
				continue;
			si->has_pw = 1;
			si->pw.luma_log2_weight_denom = 0;
			si->pw.chroma_log2_weight_denom = 0;
			for (l = 0; l < 2; l++) {
				int w = (var == VAR_BW2 && l == 1) ? 3 : 1;

				for (i = 0; i < 32; i++) {
					si->pw.weight_factors[l].luma_weight[i] = w;
					si->pw.weight_factors[l].luma_offset[i] = 0;
					for (k = 0; k < 2; k++) {
						si->pw.weight_factors[l].chroma_weight[i][k] = w;
						si->pw.weight_factors[l].chroma_offset[i][k] = 0;
					}
				}
			}
		}
	}
}

/* Limit the total number of slices across embedded streams to the live trace
 * ring capacity. H264_MAX_PICS can be raised at compile time for long-GOP cases. */

static void decode_stream(const unsigned char *data, unsigned long len, int var)
{
	struct v4l2_buffer b;
	enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT, ct = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	static struct v4l2_ctrl_h264_scaling_matrix flat;
	static struct v4l2_ctrl_h264_pred_weights nopw;
	size_t pos = 0;
	int r, i, npic = 0, nframe = 0, cap = 0;
	unsigned long w, h;

	{ unsigned char *p = (unsigned char *)&flat; for (i = 0; i < (int)sizeof(flat); i++) p[i] = 16; }
	zero(&hctx, sizeof hctx);
	hctx.hdr_abs = 1;
	r = h264_next_picture(&hctx, data, len, &pos, &pic, 1000);
	if (r != 1) { say("cannot parse the stream", r); return; }
	w = (pic.sps.pic_width_in_mbs_minus1 + 1) * 16;
	h = (pic.sps.pic_height_in_map_units_minus1 + 1) * 16 *
	    ((pic.sps.flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ? 1 : 2);
	say("STREAM size", w * 65536 + h);
	/* Each stream owns fresh capture/MV storage, including equal-sized streams. */
	setup(w, h);
	{	/* decode mode and start code: what this driver offers */
		struct v4l2_ext_control c[2];
		struct v4l2_ext_controls cs;

		zero(c, sizeof c); zero(&cs, sizeof cs);
		c[0].id = V4L2_CID_STATELESS_H264_DECODE_MODE; c[0].value = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED;
		c[1].id = V4L2_CID_STATELESS_H264_START_CODE; c[1].value = V4L2_STATELESS_H264_START_CODE_ANNEX_B;
		cs.which = V4L2_CTRL_WHICH_CUR_VAL; cs.count = 2; cs.controls = c;
		ioc(vfd, VIDIOC_S_EXT_CTRLS, &cs);
	}
	zero(&hctx, sizeof hctx);
	hctx.hdr_abs = 1;
	pos = 0;
	while (npic < H264_MAX_PICS && nframe < H264_MAX_FRAMES &&
	    (r = h264_next_picture(&hctx, data, len, &pos, &pic, (nframe + 1) * 1000)) == 1) {
		unsigned s;
		/* a frame is complete after a frame picture or the second field */
		int first_field = pic.field_pic && !pic.second_field;

		if (!pic.second_field)
			cap = nframe;
		apply_variant(&pic, var);
		for (s = 0; s < pic.nslices; s++) {
			int rfd, bad;
			struct { int fd; } ra;
			struct v4l2_ext_control ec[6];
			struct v4l2_ext_controls ecs;
			struct { int fd; short ev, rev; } pfd;
			struct { long s, n; } ts;
			int last = (s + 1 == pic.nslices);
			int hold = !last || first_field;	/* the capture buffer stays for the next slice or the second field */
			unsigned nc = 0;

			zero(&ra, sizeof ra);
			ioc(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra);
			rfd = ra.fd;
			zero(ec, sizeof ec);
			ec[nc].id = V4L2_CID_STATELESS_H264_SPS; ec[nc].ptr = &pic.sps; ec[nc].size = sizeof pic.sps; nc++;
			ec[nc].id = V4L2_CID_STATELESS_H264_PPS; ec[nc].ptr = &pic.pps; ec[nc].size = sizeof pic.pps; nc++;
			ec[nc].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
			ec[nc].ptr = pic.has_sm ? (void *)&pic.sm : (void *)&flat; ec[nc].size = sizeof flat; nc++;
			ec[nc].id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS;
			ec[nc].ptr = pic.slice[s].has_pw ? (void *)&pic.slice[s].pw : (void *)&nopw; ec[nc].size = sizeof nopw; nc++;
			ec[nc].id = V4L2_CID_STATELESS_H264_SLICE_PARAMS; ec[nc].ptr = &pic.slice[s].sp; ec[nc].size = sizeof pic.slice[s].sp; nc++;
			ec[nc].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS; ec[nc].ptr = &pic.dp; ec[nc].size = sizeof pic.dp; nc++;
			zero(&ecs, sizeof ecs);
			ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL; ecs.request_fd = rfd; ecs.count = nc; ecs.controls = ec;
			r = ioc(vfd, VIDIOC_S_EXT_CTRLS, &ecs);
			if (r != 0) {
				say("S_EXT_CTRLS failed", r); say(" error_idx", ecs.error_idx);
				sc(57, rfd, 0, 0, 0, 0, 0);
				return;
			}

			{ unsigned char *d = obuf; const unsigned char *sdat = pic.slice[s].data; for (i = 0; i < (int)pic.slice[s].len; i++) d[i] = sdat[i]; }
			if (s == 0 && !pic.second_field) {
				zero(&b, sizeof b); b.type = ct; b.memory = V4L2_MEMORY_MMAP; b.index = cap;
				r = ioc(vfd, VIDIOC_QBUF, &b);
				if (r != 0) say("QBUF cap failed", r);
			}
			zero(&b, sizeof b); b.type = ot; b.memory = V4L2_MEMORY_MMAP; b.index = 0;
			b.bytesused = pic.slice[s].len;
			b.flags = V4L2_BUF_FLAG_REQUEST_FD | (hold ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0);
			b.request_fd = rfd;
			b.timestamp.tv_sec = 0; b.timestamp.tv_usec = nframe + 1;
			r = ioc(vfd, VIDIOC_QBUF, &b);
			if (r != 0) say("QBUF out failed", r);
			if (!streaming) {
				ioc(vfd, VIDIOC_STREAMON, &ot);
				ioc(vfd, VIDIOC_STREAMON, &ct);
				streaming = 1;
			}
			r = ioc(rfd, MEDIA_REQUEST_IOC_QUEUE, 0);
			if (r != 0) say("REQUEST_QUEUE failed", r);
			pfd.fd = rfd; pfd.ev = 0x002; pfd.rev = 0;
			ts.s = 6; ts.n = 0;
			r = sc(73, (long)&pfd, 1, (long)&ts, 0, 8, 0);
			if (r != 1)
				fail("REQUEST wait failed", r);
			zero(&b, sizeof b); b.type = ot; b.memory = V4L2_MEMORY_MMAP;
			r = ioc(vfd, VIDIOC_DQBUF, &b);
			if (r != 0)
				fail("DQBUF output failed", r);
			bad = b.flags & V4L2_BUF_FLAG_ERROR;
			if (!hold || bad) {
				zero(&b, sizeof b); b.type = ct; b.memory = V4L2_MEMORY_MMAP;
				r = ioc(vfd, VIDIOC_DQBUF, &b);
				if (r != 0)
					fail("DQBUF capture failed", r);
				bad |= b.flags & V4L2_BUF_FLAG_ERROR;
			}
			sc(57, rfd, 0, 0, 0, 0, 0);
			if (bad) {
				say("DECODE failed", bad);
				return;
			}
		}
		if (!first_field) {	/* FNV-1a of the visible luma of the whole frame */
			unsigned long hsh = 2166136261UL;
			unsigned char *p = cbuf[cap];
			unsigned long y, x;

			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++)
					hsh = ((hsh ^ p[y * stride + x]) * 16777619UL) & 0xffffffffUL;
			out("PIC"); hex(nframe); hex(pic.poc); hex(pic.slice[0].sp.slice_type); hex(hsh);
			hex(pic.field_pic + 2 * pic.mbaff + 4 * pic.mmco5 + 8 * pic.idr);
			out("\n");
			nframe++;
		}
		npic++;
	}
	say("pictures decoded", npic);
}

void __attribute__((noreturn)) _start_c(void)
{
	int i;

	sc(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	{ int kk = openf("/dev/kmsg", 1); if (kk >= 0) kfd = kk; }
	out("\ninit: A64 VE H.264 trace rig\n");
	sc(101, (long)&(struct { long s, n; }){ 1, 0 }, 0, 0, 0, 0, 0);
	/* A failed held slice need not release CAPTURE. Do not block forever
	 * in DQBUF after request completion: report the error and preserve trace. */
	vfd = openf("/dev/video0", 2 | 04000); /* O_RDWR | O_NONBLOCK */
	mfd = openf("/dev/media0", 2);
	say("video0 fd", vfd); say("media0 fd", mfd);
	if (vfd < 0 || mfd < 0) fail("no device", vfd < 0 ? vfd : mfd);
	for (i = 0; i < H264_NSTREAMS; i++) {
		say("STREAM", i);
		say(" variant", h264_streams[i].variant);
		decode_stream(h264_streams[i].data, h264_streams[i].len, h264_streams[i].variant);
	}
	out("TRACE-DONE\n");
	idle();
}

__asm__(".globl _start\n_start:\n mov x0, sp\n and sp, x0, #-16\n bl _start_c\n");
