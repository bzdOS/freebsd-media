/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ffplane: FFmpeg decodes H.264 or MPEG-2 on the video engine through our V4L2 driver and the picture goes
 * to the screen on the display engine's video plane: no copy, no GPU, no render node.
 *
 * Like ffkms.c the decoder (Kwiboo's v4l2-request hwaccel in libavcodec) hands out AV_PIX_FMT_DRM_PRIME
 * frames, an AVDRMFrameDescriptor naming the dma-buf of the capture buffer and the layout of its two NV12
 * planes. Each dma-buf is imported into the KMS node (bzkms) once (drmPrimeFDToHandle, an NV12 framebuffer) and
 * every frame is put on the NV12 overlay plane with one atomic commit: EL2 latches the plane's registers at
 * the next vblank, scales the picture with the VSU and converts it with the CSC. The flip event is the
 * vblank. The frame stays referenced (so FFmpeg does not give the buffer back to the decoder) while it is
 * on screen or staged, and for one vblank after the next picture has replaced it.
 *
 * With /dev/mem readable the scanout registers of the hypervisor (SCANOUT_R_VID_*) are checked after every
 * flip: the staged addresses are the ones of the frame, they lie inside the engine's buffer pool, the
 * applied count advanced by one, VID_REJECT is still 0.
 *
 *   ffplane [-s seconds] [-a full|window|x,y,w,h] [-S] [-c 601|709] stream
 *   -a  where the picture goes (default the whole screen), -S stretch to the area (default keeps the aspect)
 * Loops the stream for `seconds` (default 10); prints the frame rate, the flip statistics and the CPU time.
 */
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define	SCANOUT_MMIO_BASE	0x0A002000UL
#define	SC_WIDTH		0x10
#define	SC_HEIGHT		0x14
#define	SC_VBLANK_COUNT		0x40
#define	SC_VID_ADDR0		0xA0
#define	SC_VID_ADDR1		0xA4
#define	SC_VID_PITCH0		0xA8
#define	SC_VID_PITCH1		0xAC
#define	SC_VID_SRC_SIZE		0xB0
#define	SC_VID_DST_COORD	0xB4
#define	SC_VID_DST_SIZE		0xB8
#define	SC_VID_FORMAT		0xBC
#define	SC_VID_CTRL		0xC0
#define	SC_VID_COUNT		0xC4
#define	SC_VID_REJECT		0xC8

#define	NBUF	32
#define	NRET	4

struct buf {
	int		fd;
	uint32_t	handle, fb;
	uint32_t	pa0, pa1;	/* what EL2 was given, learnt at the first flip */
};

static struct {
	int		fd, waiting, have_regs, bt709, stretch;
	drmModeModeInfo	mode;
	uint32_t	crtc, conn, plane, dumb_fb;
	uint32_t	p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_dx, p_dy, p_dw, p_dh, p_enc;
	uint64_t	enc_val;
	volatile uint32_t *reg;
	unsigned	scrw, scrh;
	const char	*area;
	int		dx, dy;
	unsigned	dw, dh, sw, sh;
	struct buf	buf[NBUF];
	int		nbuf;
	AVFrame		*pending, *front;
	struct buf	*pending_buf;
	AVFrame		*ret[NRET];
	uint32_t	ret_vbl[NRET];
	uint64_t	pool_lo, pool_hi;
	double		t0, t_last;
	uint32_t	vb0, vb_last, cnt0, rej0, cnt_exp;
	unsigned long	frames, events, mism, outside, gaps[4], skipped;
	double		gap_sum, gap_min, gap_max;
} K = { .gap_min = 1e9 };

static void
die(const char *m)
{

	fprintf(stderr, "ffplane: %s", m);
	if (errno != 0)
		fprintf(stderr, ": %s", strerror(errno));
	fputc('\n', stderr);
	exit(1);
}

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

static inline uint32_t
rd(unsigned off)
{

	return (K.reg[off / 4]);
}

static uint32_t
vblanks(void)
{

	return (K.have_regs ? rd(SC_VBLANK_COUNT) : 0);
}

static void
flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{

	*(int *)data = 0;
}

static int
open_kms(void)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 8; i++) {
		drmModeRes *res;
		int j, found = 0;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		if ((fd = open(path, O_RDWR)) < 0)
			continue;
		if ((res = drmModeGetResources(fd)) != NULL) {
			for (j = 0; j < res->count_connectors; j++) {
				drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[j]);

				if (cn != NULL) {
					if (cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0)
						found = 1;
					drmModeFreeConnector(cn);
				}
			}
			drmModeFreeResources(res);
		}
		if (found) {
			printf("KMS node %s\n", path);
			return (fd);
		}
		close(fd);
	}
	return (-1);
}

static uint32_t
prop_id(uint32_t obj, const char *name, const char *val_name, uint64_t *val)
{
	drmModeObjectProperties *pr = drmModeObjectGetProperties(K.fd, obj, DRM_MODE_OBJECT_PLANE);
	uint32_t id = 0;
	unsigned i;

	if (pr == NULL)
		return (0);
	for (i = 0; i < pr->count_props && id == 0; i++) {
		drmModePropertyRes *p = drmModeGetProperty(K.fd, pr->props[i]);

		if (p != NULL && strcmp(p->name, name) == 0) {
			id = p->prop_id;
			if (val_name != NULL && val != NULL) {
				int k;

				for (k = 0; k < p->count_enums; k++)
					if (strcmp(p->enums[k].name, val_name) == 0)
						*val = p->enums[k].value;
			}
		}
		if (p != NULL)
			drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(pr);
	return (id);
}

static void
place(void)
{
	int ax = 0, ay = 0, t1, t2;
	unsigned aw = K.scrw, ah = K.scrh, dw, dh, t3, t4;

	if (K.area != NULL && strcmp(K.area, "full") != 0) {
		if (strcmp(K.area, "window") == 0) {
			ax = 16; ay = 66; aw = 1120; ah = 276;
		} else if (sscanf(K.area, "%d,%d,%u,%u", &t1, &t2, &t3, &t4) == 4) {
			ax = t1; ay = t2; aw = t3; ah = t4;
		} else
			die("bad -a");
	}
	dw = aw;
	dh = ah;
	if (!K.stretch) {
		if ((uint64_t)aw * K.sh > (uint64_t)ah * K.sw)
			dw = (unsigned)((uint64_t)ah * K.sw / K.sh);
		else
			dh = (unsigned)((uint64_t)aw * K.sh / K.sw);
	}
	if (K.sw > 2 * dw || K.sh > 2 * dh) {
		unsigned nw = dw < (K.sw + 1) / 2 ? (K.sw + 1) / 2 : dw;
		unsigned nh = dh < (K.sh + 1) / 2 ? (K.sh + 1) / 2 : dh;

		printf("destination %ux%u is below half the %ux%u picture (refused by the hypervisor): "
		    "enlarged to %ux%u\n", dw, dh, K.sw, K.sh, nw, nh);
		dw = nw;
		dh = nh;
	}
	K.dx = ax + (int)(aw - dw) / 2;
	K.dy = ay + (int)(ah - dh) / 2;
	K.dw = dw;
	K.dh = dh;
	/* An enlarged destination can poke out of the top or left of its area; the plane may not. */
	if (K.dx < 0)
		K.dx = 0;
	if (K.dy < 0)
		K.dy = 0;
}

static int
commit(struct buf *b, uint32_t flags, void *ud)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int r;

	drmModeAtomicAddProperty(req, K.plane, K.p_fb, b != NULL ? b->fb : 0);
	drmModeAtomicAddProperty(req, K.plane, K.p_crtc, b != NULL ? K.crtc : 0);
	if (b != NULL) {
		drmModeAtomicAddProperty(req, K.plane, K.p_sx, 0);
		drmModeAtomicAddProperty(req, K.plane, K.p_sy, 0);
		drmModeAtomicAddProperty(req, K.plane, K.p_sw, (uint64_t)K.sw << 16);
		drmModeAtomicAddProperty(req, K.plane, K.p_sh, (uint64_t)K.sh << 16);
		drmModeAtomicAddProperty(req, K.plane, K.p_dx, K.dx);
		drmModeAtomicAddProperty(req, K.plane, K.p_dy, K.dy);
		drmModeAtomicAddProperty(req, K.plane, K.p_dw, K.dw);
		drmModeAtomicAddProperty(req, K.plane, K.p_dh, K.dh);
		if (K.p_enc != 0)
			drmModeAtomicAddProperty(req, K.plane, K.p_enc, K.enc_val);
	}
	r = drmModeAtomicCommit(K.fd, req, flags, ud);
	drmModeAtomicFree(req);
	return (r);
}

static void
wait_event(void)
{
	struct pollfd pfd = { .fd = K.fd, .events = POLLIN };
	drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };
	double t0 = now_s();

	while (K.waiting) {
		if (poll(&pfd, 1, 1000) <= 0 || now_s() - t0 > 3) {
			fprintf(stderr, "flip event did not arrive\n");
			K.waiting = 0;
			K.mism++;
			return;
		}
		drmHandleEvent(K.fd, &ev);
	}
}

/* Give back the frames whose retire vblank has passed. */
static void
settle(int all)
{
	int i;

	for (i = 0; i < NRET; i++)
		if (K.ret[i] != NULL && (all || (int32_t)(vblanks() - K.ret_vbl[i]) >= 0))
			av_frame_free(&K.ret[i]);
}

static void
retire(AVFrame *f, uint32_t vbl)
{
	int i;

	for (;;) {
		for (i = 0; i < NRET; i++)
			if (K.ret[i] == NULL) {
				K.ret[i] = f;
				K.ret_vbl[i] = vbl;
				return;
			}
		/* All slots busy: the oldest must have been on screen long enough. */
		usleep(2000);
		settle(0);
	}
}

/* The flip of the pending frame has completed. */
static void
confirmed(void)
{
	struct buf *b = K.pending_buf;
	uint32_t vb = vblanks();
	double t = now_s();

	if (K.have_regs) {
		int bad = 0;

		K.cnt_exp++;
		if (b->pa0 == 0) {
			b->pa0 = rd(SC_VID_ADDR0);
			b->pa1 = rd(SC_VID_ADDR1);
			if (b->pa0 < K.pool_lo || b->pa1 + K.sh / 2 * rd(SC_VID_PITCH1) > K.pool_hi ||
			    b->pa1 < b->pa0)
				K.outside++;
		}
		if (rd(SC_VID_ADDR0) != b->pa0 || rd(SC_VID_ADDR1) != b->pa1 ||
		    rd(SC_VID_SRC_SIZE) != (K.sw | K.sh << 16) ||
		    rd(SC_VID_DST_COORD) != ((uint32_t)K.dx | (uint32_t)K.dy << 16) ||
		    rd(SC_VID_DST_SIZE) != (K.dw | K.dh << 16) || rd(SC_VID_FORMAT) != 0 ||
		    rd(SC_VID_CTRL) != (1u | (K.bt709 ? 2u : 0u)) ||
		    rd(SC_VID_COUNT) - K.cnt0 != K.cnt_exp || rd(SC_VID_REJECT) != K.rej0)
			bad = 1;
		if (bad && K.mism++ < 5)
			fprintf(stderr, "frame %lu: registers addr0 %#x addr1 %#x count %u (expected %u) "
			    "reject %u\n", K.frames, rd(SC_VID_ADDR0), rd(SC_VID_ADDR1),
			    rd(SC_VID_COUNT) - K.cnt0, K.cnt_exp, rd(SC_VID_REJECT) - K.rej0);
	}
	if (K.events > 0) {
		double gap = (t - K.t_last) * 1000;
		uint32_t dv = vb - K.vb_last;

		K.gap_sum += gap;
		if (gap < K.gap_min)
			K.gap_min = gap;
		if (gap > K.gap_max)
			K.gap_max = gap;
		if (K.have_regs) {
			K.gaps[dv > 3 ? 3 : dv]++;
			if (dv > 1)
				K.skipped += dv - 1;
		}
	}
	K.t_last = t;
	K.vb_last = vb;
	K.events++;
	if (K.front != NULL)
		retire(K.front, vb + 1);
	K.front = K.pending;
	K.pending = NULL;
	K.pending_buf = NULL;
}

static struct buf *
find_buf(const AVFrame *f)
{
	const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
	const AVDRMLayerDescriptor *l = &d->layers[0];
	int fd = d->objects[l->planes[0].object_index].fd, i;
	struct buf *b;
	uint32_t hs[4] = { 0 }, ps[4] = { 0 }, os[4] = { 0 };

	for (i = 0; i < K.nbuf; i++)
		if (K.buf[i].fd == fd)
			return (&K.buf[i]);
	if (K.nbuf == NBUF)
		die("too many distinct frame buffers");
	if (d->nb_layers != 1 || l->format != DRM_FORMAT_NV12 || l->nb_planes != 2 ||
	    d->objects[l->planes[0].object_index].fd != d->objects[l->planes[1].object_index].fd)
		die("the frame is not a one-object NV12 layer");
	b = &K.buf[K.nbuf++];
	b->fd = fd;
	if (drmPrimeFDToHandle(K.fd, fd, &b->handle) != 0)
		die("drmPrimeFDToHandle");
	hs[0] = hs[1] = b->handle;
	ps[0] = l->planes[0].pitch;
	ps[1] = l->planes[1].pitch;
	os[0] = l->planes[0].offset;
	os[1] = l->planes[1].offset;
	if (drmModeAddFB2(K.fd, f->width, f->height, DRM_FORMAT_NV12, hs, ps, os, &b->fb, 0) != 0)
		die("drmModeAddFB2 NV12");
	return (b);
}

static void
plane_init(unsigned w, unsigned h)
{
	drmModeRes *res;
	drmModeConnector *cn = NULL;
	drmModePlaneRes *pres;
	struct drm_mode_create_dumb cd;
	uint32_t hs[4] = { 0 }, ps[4] = { 0 }, os[4] = { 0 };
	unsigned i;
	int mfd;
	FILE *p;
	char line[256];

	K.sw = w;
	K.sh = h;
	if ((K.fd = open_kms()) < 0)
		die("no KMS node (is bzkms loaded?)");
	drmSetClientCap(K.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (drmSetClientCap(K.fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
		die("no atomic KMS");
	res = drmModeGetResources(K.fd);
	for (i = 0; res != NULL && i < (unsigned)res->count_connectors && cn == NULL; i++) {
		drmModeConnector *c = drmModeGetConnector(K.fd, res->connectors[i]);

		if (c != NULL && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
			cn = c;
		else if (c != NULL)
			drmModeFreeConnector(c);
	}
	if (cn == NULL || res->count_crtcs < 1)
		die("no connector or CRTC");
	K.conn = cn->connector_id;
	K.mode = cn->modes[0];
	K.crtc = res->crtcs[0];
	pres = drmModeGetPlaneResources(K.fd);
	for (i = 0; pres != NULL && i < pres->count_planes && K.plane == 0; i++) {
		drmModePlane *pl = drmModeGetPlane(K.fd, pres->planes[i]);
		unsigned f;

		if (pl == NULL)
			continue;
		for (f = 0; f < pl->count_formats; f++)
			if (pl->formats[f] == DRM_FORMAT_NV12)
				K.plane = pl->plane_id;
		drmModeFreePlane(pl);
	}
	if (K.plane == 0)
		die("no plane takes NV12 (bzkms without the video plane?)");
	K.p_fb = prop_id(K.plane, "FB_ID", NULL, NULL);
	K.p_crtc = prop_id(K.plane, "CRTC_ID", NULL, NULL);
	K.p_sx = prop_id(K.plane, "SRC_X", NULL, NULL);
	K.p_sy = prop_id(K.plane, "SRC_Y", NULL, NULL);
	K.p_sw = prop_id(K.plane, "SRC_W", NULL, NULL);
	K.p_sh = prop_id(K.plane, "SRC_H", NULL, NULL);
	K.p_dx = prop_id(K.plane, "CRTC_X", NULL, NULL);
	K.p_dy = prop_id(K.plane, "CRTC_Y", NULL, NULL);
	K.p_dw = prop_id(K.plane, "CRTC_W", NULL, NULL);
	K.p_dh = prop_id(K.plane, "CRTC_H", NULL, NULL);
	K.p_enc = prop_id(K.plane, "COLOR_ENCODING",
	    K.bt709 ? "ITU-R BT.709 YCbCr" : "ITU-R BT.601 YCbCr", &K.enc_val);
	printf("KMS: connector %u, crtc %u, mode %ux%u@%u, video plane %u\n", K.conn, K.crtc,
	    K.mode.hdisplay, K.mode.vdisplay, K.mode.vrefresh, K.plane);

	K.scrw = 1920;
	K.scrh = 1080;
	if ((mfd = open("/dev/mem", O_RDWR)) >= 0) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, SCANOUT_MMIO_BASE);

		if (m != MAP_FAILED && ((volatile uint32_t *)m)[0] == 0x53434e41u) {
			K.reg = m;
			K.have_regs = 1;
			K.scrw = rd(SC_WIDTH);
			K.scrh = rd(SC_HEIGHT);
			printf("scanout registers mapped, screen %ux%u\n", K.scrw, K.scrh);
		}
	}
	/* The engine's buffer pool, from the log: every capture buffer must be inside it. */
	K.pool_lo = 0;
	K.pool_hi = ~0ULL;
	if ((p = popen("dmesg | grep 'buffer pool: ' | tail -1", "r")) != NULL) {
		if (fgets(line, sizeof(line), p) != NULL) {
			unsigned mb;
			unsigned long long pa;
			char *e = strstr(line, "buffer pool: ");

			if (e != NULL && sscanf(e, "buffer pool: %u MiB at pa %llx", &mb, &pa) == 2) {
				K.pool_lo = pa;
				K.pool_hi = pa + ((uint64_t)mb << 20);
				printf("buffer pool %#llx..%#llx\n", pa, (unsigned long long)K.pool_hi);
			}
		}
		pclose(p);
	}
	place();

	memset(&cd, 0, sizeof(cd));
	cd.width = K.mode.hdisplay;
	cd.height = K.mode.vdisplay;
	cd.bpp = 32;
	if (drmIoctl(K.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0)
		die("CREATE_DUMB");
	hs[0] = cd.handle;
	ps[0] = cd.pitch;
	if (drmModeAddFB2(K.fd, K.mode.hdisplay, K.mode.vdisplay, DRM_FORMAT_XRGB8888, hs, ps, os, &K.dumb_fb,
	    0) != 0)
		die("AddFB2 window");
	if (drmModeSetCrtc(K.fd, K.crtc, K.dumb_fb, 0, 0, &K.conn, 1, &K.mode) != 0)
		die("SetCrtc");
	if (K.have_regs) {
		K.cnt0 = rd(SC_VID_COUNT);
		K.rej0 = rd(SC_VID_REJECT);
		K.vb0 = vblanks();
	}
}

static int
show(AVFrame *f)
{
	struct buf *b;

	b = find_buf(f);
	settle(0);
	if (K.waiting)
		wait_event();
	if (K.pending != NULL)
		confirmed();
	K.waiting = 1;
	if (commit(b, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &K.waiting) != 0) {
		fprintf(stderr, "atomic commit: %s\n", strerror(errno));
		K.waiting = 0;
		return (1);
	}
	K.pending = av_frame_clone(f);
	K.pending_buf = b;
	K.frames++;
	return (0);
}

static enum AVPixelFormat
get_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
	for (; *fmts != AV_PIX_FMT_NONE; fmts++)
		if (*fmts == AV_PIX_FMT_DRM_PRIME)
			return (*fmts);
	return (AV_PIX_FMT_NONE);
}

static double
cpu(const struct rusage *r)
{

	return (r->ru_utime.tv_sec + r->ru_utime.tv_usec * 1e-6 + r->ru_stime.tv_sec + r->ru_stime.tv_usec * 1e-6);
}

int
main(int argc, char **argv)
{
	AVFormatContext *fc = NULL;
	AVCodecContext *cc;
	const AVCodec *codec;
	AVBufferRef *dev = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	double secs = 10, dt;
	struct rusage ru0, ru1;
	int ch, vs, ret, first = 1, force = 0, i;
	unsigned long decoded = 0, last_decoded = 0;
	int eof_spin = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((ch = getopt(argc, argv, "s:a:Sc:")) != -1) {
		switch (ch) {
		case 's': secs = atof(optarg); break;
		case 'a': K.area = optarg; break;
		case 'S': K.stretch = 1; break;
		case 'c': force = atoi(optarg); break;
		default: return (2);
		}
	}
	if (argc - optind != 1)
		die("usage: ffplane [-s seconds] [-a full|window|x,y,w,h] [-S] [-c 601|709] stream");
	if (avformat_open_input(&fc, argv[optind], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
		die("cannot open the stream");
	vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (vs < 0)
		die("no video stream");
	cc = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar);
	if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0)
		die("no V4L2 Request device");
	cc->hw_device_ctx = av_buffer_ref(dev);
	cc->get_format = get_format;
	if (avcodec_open2(cc, codec, NULL) < 0)
		die("cannot open the decoder");
	printf("%s %dx%d\n", codec->name, cc->width, cc->height);
	getrusage(RUSAGE_SELF, &ru0);
	K.t0 = now_s();
	while ((dt = now_s() - K.t0) < secs) {
		ret = av_read_frame(fc, pkt);
		if (ret == AVERROR_EOF) {
			/* Loop the stream: a timestamp seek for containers, a byte seek for raw streams. */
			if (decoded == last_decoded && ++eof_spin > 3)
				die("the stream will not restart");
			last_decoded = decoded;
			avcodec_flush_buffers(cc);
			if (av_seek_frame(fc, vs, 0, AVSEEK_FLAG_BACKWARD) < 0 &&
			    av_seek_frame(fc, vs, 0, AVSEEK_FLAG_BYTE) < 0)
				break;
			continue;
		}
		if (ret < 0)
			break;
		if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) >= 0)
			while (avcodec_receive_frame(cc, fr) >= 0) {
				decoded++;
				if (fr->format != AV_PIX_FMT_DRM_PRIME)
					die("the decoder did not give DRM_PRIME frames");
				if (first) {
					K.bt709 = force != 0 ? force == 709 : fr->colorspace == AVCOL_SPC_BT709 ||
					    (fr->colorspace == AVCOL_SPC_UNSPECIFIED && fr->height >= 720);
					plane_init(fr->width, fr->height);
					printf("picture %dx%d -> %ux%u at %d,%d, BT.%s\n", fr->width, fr->height, K.dw,
					    K.dh, K.dx, K.dy, K.bt709 ? "709" : "601");
					first = 0;
					K.t0 = now_s();
					decoded = 0;
					getrusage(RUSAGE_SELF, &ru0);
				}
				if (show(fr))
					return (1);
				av_frame_unref(fr);
			}
		av_packet_unref(pkt);
	}
	if (K.waiting)
		wait_event();
	if (K.pending != NULL)
		confirmed();
	dt = now_s() - K.t0;
	getrusage(RUSAGE_SELF, &ru1);
	printf("%lu frames decoded, %lu shown in %.2f s = %.2f fps; flip events %lu\n", decoded, K.frames, dt,
	    K.frames / dt, K.events);
	printf("process CPU %.3f s of %.2f s = %.1f%% of one core\n", cpu(&ru1) - cpu(&ru0), dt,
	    100 * (cpu(&ru1) - cpu(&ru0)) / dt);
	if (K.events > 1)
		printf("flip interval ms: mean %.2f min %.2f max %.2f\n", K.gap_sum / (K.events - 1), K.gap_min,
		    K.gap_max);
	if (K.have_regs) {
		printf("EL2: %u vblanks in the run, VID applied %u (expected %u), VID_REJECT %u, register "
		    "mismatches %lu, buffers outside the pool %lu\n", vblanks() - K.vb0, rd(SC_VID_COUNT) - K.cnt0,
		    K.cnt_exp, rd(SC_VID_REJECT) - K.rej0, K.mism, K.outside);
		printf("vblanks between consecutive flips: 1 x %lu, 2 x %lu, 3+ x %lu (0 x %lu); skipped %lu\n",
		    K.gaps[1], K.gaps[2], K.gaps[3], K.gaps[0], K.skipped);
	}
	printf("%d distinct capture buffers shown\n", K.nbuf);
	for (i = 0; i < K.nbuf; i++)
		printf("  buffer %d: luma %#x chroma %#x\n", i, K.buf[i].pa0, K.buf[i].pa1);
	/* Plane off, window back to the hypervisor. */
	commit(NULL, 0, NULL);
	drmModeSetCrtc(K.fd, K.crtc, 0, 0, 0, NULL, 0, NULL);
	if (K.have_regs)
		printf("after cleanup VID_CTRL %#x\n", rd(SC_VID_CTRL));
	settle(1);
	if (K.front != NULL)
		av_frame_free(&K.front);
	for (i = 0; i < K.nbuf; i++)
		drmModeRmFB(K.fd, K.buf[i].fb);
	fflush(stdout);
	_exit(0);
}
