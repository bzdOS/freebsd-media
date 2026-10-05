/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vtest: a virtual capture device that produces test patterns.
 *
 * It exists to exercise the media core without hardware: formats (YUYV and
 * RGB24, any even size from 160x120 to 1920x1080), a frame rate, controls,
 * and a stream of frames made by a callout. The frame is eight colour bars, or
 * a gradient, or black, with a white marker that moves one step per frame so
 * that consecutive frames differ and a test can tell them apart.
 *
 * Controls: Brightness (shifts the luma), Contrast, Saturation, Hue (stored,
 * not applied), Horizontal Flip (mirrors the bars), Test Pattern (menu).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>

#include <dev/media/media.h>

#define	VTEST_MIN_W	160
#define	VTEST_MIN_H	120
#define	VTEST_MAX_W	1920
#define	VTEST_MAX_H	1080
#define	VTEST_STEP	2

static const uint32_t vtest_pixfmts[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24 };

/* Frame rates, as frame periods (numerator/denominator seconds). */
static const struct v4l2_fract vtest_intervals[] = {
	{ 1, 30 }, { 1, 15 }, { 1, 10 }, { 1, 5 }, { 1, 1 },
};

static const char * const vtest_patterns[] = {
	"Color Bars", "Horizontal Gradient", "Black",
};

struct vtest_softc {
	device_t		dev;
	struct vdev		vdev;
	struct vbq		vbq;
	struct vctrl_handler	ctrls;
	struct vctrl		*c_bright, *c_hflip, *c_pattern;
	struct mtx		mtx;		/* format, ring, frame counter */
	struct callout		callout;
	/* current format */
	uint32_t		width, height, pixfmt;
	uint32_t		bpl, sizeimage;
	struct v4l2_fract	interval;
	/* buffers the core gave us, oldest first */
	struct vbuf		*ring[VBQ_MAX_BUFFERS];
	u_int			head, count;
	uint32_t		frame;
	sbintime_t		next;		/* deadline of the next frame */
};

/* ----------------------------------------------------------------- format */

static u_int
vtest_bpp(uint32_t pixfmt)
{

	return (pixfmt == V4L2_PIX_FMT_RGB24 ? 3 : 2);
}

static bool
vtest_known_fmt(uint32_t pixfmt)
{
	u_int i;

	for (i = 0; i < nitems(vtest_pixfmts); i++)
		if (vtest_pixfmts[i] == pixfmt)
			return (true);
	return (false);
}

static uint32_t
vtest_clamp_size(uint32_t v, uint32_t min, uint32_t max)
{

	if (v < min)
		v = min;
	if (v > max)
		v = max;
	/* Round to the nearest step counted from the minimum. */
	v = min + ((v - min + VTEST_STEP / 2) / VTEST_STEP) * VTEST_STEP;
	return (v > max ? max : v);
}

static void
vtest_fill_fmt(struct v4l2_pix_format *p, uint32_t w, uint32_t h, uint32_t pixfmt)
{

	memset(p, 0, sizeof(*p));
	p->width = w;
	p->height = h;
	p->pixelformat = pixfmt;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = w * vtest_bpp(pixfmt);
	p->sizeimage = p->bytesperline * h;
	p->colorspace = V4L2_COLORSPACE_SRGB;
	p->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	p->quantization = V4L2_QUANTIZATION_DEFAULT;
	p->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int
vtest_try_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct v4l2_pix_format *p = &f->fmt.pix;
	uint32_t w, h, pixfmt;

	pixfmt = vtest_known_fmt(p->pixelformat) ? p->pixelformat : V4L2_PIX_FMT_YUYV;
	w = vtest_clamp_size(p->width, VTEST_MIN_W, VTEST_MAX_W);
	h = vtest_clamp_size(p->height, VTEST_MIN_H, VTEST_MAX_H);
	vtest_fill_fmt(p, w, h, pixfmt);
	return (0);
}

static int
vtest_g_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct vtest_softc *sc = __containerof(v, struct vtest_softc, vdev);

	mtx_lock(&sc->mtx);
	vtest_fill_fmt(&f->fmt.pix, sc->width, sc->height, sc->pixfmt);
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
vtest_s_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct vtest_softc *sc = __containerof(v, struct vtest_softc, vdev);

	vtest_try_fmt(v, fh, f);
	mtx_lock(&sc->mtx);
	sc->width = f->fmt.pix.width;
	sc->height = f->fmt.pix.height;
	sc->pixfmt = f->fmt.pix.pixelformat;
	sc->bpl = f->fmt.pix.bytesperline;
	sc->sizeimage = f->fmt.pix.sizeimage;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
vtest_enum_fmt(struct vdev *v, struct vfh *fh, struct v4l2_fmtdesc *f)
{

	if (f->index >= nitems(vtest_pixfmts))
		return (EINVAL);
	f->pixelformat = vtest_pixfmts[f->index];
	strlcpy((char *)f->description,
	    f->pixelformat == V4L2_PIX_FMT_RGB24 ? "24-bit RGB 8-8-8" :
	    "YUYV 4:2:2", sizeof(f->description));
	return (0);
}

static int
vtest_enum_framesizes(struct vdev *v, struct vfh *fh, struct v4l2_frmsizeenum *f)
{

	if (f->index != 0 || !vtest_known_fmt(f->pixel_format))
		return (EINVAL);
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = VTEST_MIN_W;
	f->stepwise.max_width = VTEST_MAX_W;
	f->stepwise.step_width = VTEST_STEP;
	f->stepwise.min_height = VTEST_MIN_H;
	f->stepwise.max_height = VTEST_MAX_H;
	f->stepwise.step_height = VTEST_STEP;
	memset(f->reserved, 0, sizeof(f->reserved));
	return (0);
}

static int
vtest_enum_frameintervals(struct vdev *v, struct vfh *fh, struct v4l2_frmivalenum *f)
{

	if (f->index >= nitems(vtest_intervals) || !vtest_known_fmt(f->pixel_format) ||
	    f->width < VTEST_MIN_W || f->width > VTEST_MAX_W ||
	    f->height < VTEST_MIN_H || f->height > VTEST_MAX_H ||
	    (f->width - VTEST_MIN_W) % VTEST_STEP != 0 ||
	    (f->height - VTEST_MIN_H) % VTEST_STEP != 0)
		return (EINVAL);
	f->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	f->discrete = vtest_intervals[f->index];
	memset(f->reserved, 0, sizeof(f->reserved));
	return (0);
}

/* The listed interval closest to `want`; 0/x or x/0 means the default. */
static struct v4l2_fract
vtest_nearest_interval(struct v4l2_fract want)
{
	uint64_t w, d, best = UINT64_MAX;
	u_int i, bi = 0;

	if (want.numerator == 0 || want.denominator == 0)
		return (vtest_intervals[0]);
	/* Compare periods in microseconds. */
	w = (uint64_t)want.numerator * 1000000 / want.denominator;
	for (i = 0; i < nitems(vtest_intervals); i++) {
		uint64_t p = (uint64_t)vtest_intervals[i].numerator * 1000000 /
		    vtest_intervals[i].denominator;

		d = p > w ? p - w : w - p;
		if (d < best) {
			best = d;
			bi = i;
		}
	}
	return (vtest_intervals[bi]);
}

static int
vtest_g_parm(struct vdev *v, struct vfh *fh, struct v4l2_streamparm *p)
{
	struct vtest_softc *sc = __containerof(v, struct vtest_softc, vdev);

	memset(&p->parm, 0, sizeof(p->parm));
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	mtx_lock(&sc->mtx);
	p->parm.capture.timeperframe = sc->interval;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
vtest_s_parm(struct vdev *v, struct vfh *fh, struct v4l2_streamparm *p)
{
	struct vtest_softc *sc = __containerof(v, struct vtest_softc, vdev);
	struct v4l2_fract want = p->parm.capture.timeperframe;

	memset(&p->parm, 0, sizeof(p->parm));
	p->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	mtx_lock(&sc->mtx);
	sc->interval = vtest_nearest_interval(want);
	p->parm.capture.timeperframe = sc->interval;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
vtest_querycap(struct vdev *v, struct vfh *fh, struct v4l2_capability *cap)
{

	strlcpy((char *)cap->driver, "vtest", sizeof(cap->driver));
	strlcpy((char *)cap->card, "Virtual Test Camera", sizeof(cap->card));
	strlcpy((char *)cap->bus_info, "platform:vtest", sizeof(cap->bus_info));
	return (0);
}

static int
vtest_s_ctrl(struct vdev *v, struct vfh *fh, struct vctrl *c)
{

	/* Values are read when a frame is made; nothing to program. */
	return (0);
}

/* ------------------------------------------------------------------ frames */

struct bar { uint8_t y, u, v, r, g, b; };

static const struct bar vtest_bars[8] = {
	{ 235, 128, 128, 255, 255, 255 },	/* white */
	{ 210,  16, 146, 255, 255,   0 },	/* yellow */
	{ 170, 166,  16,   0, 255, 255 },	/* cyan */
	{ 145,  54,  34,   0, 255,   0 },	/* green */
	{ 106, 202, 222, 255,   0, 255 },	/* magenta */
	{  81,  90, 240, 255,   0,   0 },	/* red */
	{  41, 240, 110,   0,   0, 255 },	/* blue */
	{  16, 128, 128,   0,   0,   0 },	/* black */
};

static uint8_t
vtest_luma_shift(int y, int bright)
{
	/* Brightness 0..255 around 128; keep the video range 16..235. */
	y += (bright - 128) / 2;
	return (y < 16 ? 16 : y > 235 ? 235 : y);
}

/* One pixel of the frame at x, as the bar it belongs to. */
static struct bar
vtest_pixel(u_int x, u_int w, int pattern, bool hflip, int bright, bool marker)
{
	struct bar b;

	if (marker) {
		b = vtest_bars[0];
	} else if (pattern == 2) {
		b = vtest_bars[7];
	} else if (pattern == 1) {
		uint8_t g = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));

		b.r = b.g = b.b = g;
		b.y = 16 + (uint8_t)((uint32_t)g * 219 / 255);
		b.u = b.v = 128;
	} else {
		u_int bar = x * 8 / w;

		b = vtest_bars[hflip ? 7 - bar : bar];
	}
	b.y = vtest_luma_shift(b.y, bright);
	return (b);
}

static void
vtest_make_frame(struct vtest_softc *sc, struct vbuf *b)
{
	uint8_t *dst = b->vaddr;
	u_int w = sc->width, h = sc->height, x, y, mx;
	int pattern = (int)vctrl_get(sc->c_pattern);
	int bright = (int)vctrl_get(sc->c_bright);
	bool hflip = vctrl_get(sc->c_hflip) != 0;

	mx = (sc->frame * 8) % w;
	for (y = 0; y < h; y++) {
		uint8_t *row = dst + (size_t)y * sc->bpl;

		for (x = 0; x < w; x += 2) {
			bool m0 = y < 16 && x >= mx && x < mx + 16;
			bool m1 = y < 16 && x + 1 >= mx && x + 1 < mx + 16;
			struct bar p0 = vtest_pixel(x, w, pattern, hflip, bright, m0);
			struct bar p1 = vtest_pixel(x + 1, w, pattern, hflip, bright, m1);

			if (sc->pixfmt == V4L2_PIX_FMT_YUYV) {
				row[2 * x + 0] = p0.y;
				row[2 * x + 1] = p0.u;
				row[2 * x + 2] = p1.y;
				row[2 * x + 3] = p0.v;
			} else {
				row[3 * x + 0] = p0.r;
				row[3 * x + 1] = p0.g;
				row[3 * x + 2] = p0.b;
				row[3 * x + 3] = p1.r;
				row[3 * x + 4] = p1.g;
				row[3 * x + 5] = p1.b;
			}
		}
	}
	b->bytesused = sc->sizeimage;
}

static sbintime_t
vtest_period(struct vtest_softc *sc)
{

	return ((sbintime_t)SBT_1S * sc->interval.numerator / sc->interval.denominator);
}

static void
vtest_tick(void *arg)
{
	struct vtest_softc *sc = arg;
	struct vbuf *b = NULL;
	sbintime_t now, period = vtest_period(sc);

	mtx_assert(&sc->mtx, MA_OWNED);
	/*
	 * Schedule from the previous deadline, not from now, so the rate does not
	 * drift by the callout latency; if we fell more than a frame behind (the
	 * machine was busy), skip ahead instead of firing a burst.
	 */
	now = sbinuptime();
	sc->next += period;
	if (sc->next < now)
		sc->next = now + period;
	callout_reset_sbt(&sc->callout, sc->next, 0, vtest_tick, sc, C_ABSOLUTE);
	if (sc->count == 0)
		return;			/* no buffer: this frame is dropped */
	b = sc->ring[sc->head];
	sc->head = (sc->head + 1) % VBQ_MAX_BUFFERS;
	sc->count--;
	vtest_make_frame(sc, b);
	sc->frame++;
	vbq_buf_done(b, false);
}

/* ------------------------------------------------------------ buffer queue */

static int
vtest_queue_setup(struct vbq *q, uint32_t *nbuffers, size_t *size)
{
	struct vtest_softc *sc = __containerof(q, struct vtest_softc, vbq);

	if (*nbuffers < 2)
		*nbuffers = 2;
	mtx_lock(&sc->mtx);
	*size = sc->sizeimage;
	mtx_unlock(&sc->mtx);
	return (0);
}

static void
vtest_buf_queue(struct vbq *q, struct vbuf *b)
{
	struct vtest_softc *sc = __containerof(q, struct vtest_softc, vbq);

	mtx_lock(&sc->mtx);
	sc->ring[(sc->head + sc->count) % VBQ_MAX_BUFFERS] = b;
	sc->count++;
	mtx_unlock(&sc->mtx);
}

static int
vtest_start_streaming(struct vbq *q, uint32_t count)
{
	struct vtest_softc *sc = __containerof(q, struct vtest_softc, vbq);

	mtx_lock(&sc->mtx);
	sc->frame = 0;
	sc->next = sbinuptime() + vtest_period(sc);
	callout_reset_sbt(&sc->callout, sc->next, 0, vtest_tick, sc, C_ABSOLUTE);
	mtx_unlock(&sc->mtx);
	return (0);
}

static void
vtest_stop_streaming(struct vbq *q)
{
	struct vtest_softc *sc = __containerof(q, struct vtest_softc, vbq);

	callout_drain(&sc->callout);
	mtx_lock(&sc->mtx);
	sc->head = sc->count = 0;
	mtx_unlock(&sc->mtx);
}

static kobj_method_t vtest_vdev_methods[] = {
	KOBJMETHOD(vdev_querycap,		vtest_querycap),
	KOBJMETHOD(vdev_enum_fmt,		vtest_enum_fmt),
	KOBJMETHOD(vdev_g_fmt,			vtest_g_fmt),
	KOBJMETHOD(vdev_try_fmt,		vtest_try_fmt),
	KOBJMETHOD(vdev_s_fmt,			vtest_s_fmt),
	KOBJMETHOD(vdev_enum_framesizes,	vtest_enum_framesizes),
	KOBJMETHOD(vdev_enum_frameintervals,	vtest_enum_frameintervals),
	KOBJMETHOD(vdev_g_parm,			vtest_g_parm),
	KOBJMETHOD(vdev_s_parm,			vtest_s_parm),
	KOBJMETHOD(vdev_s_ctrl,			vtest_s_ctrl),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(vtest_vdev, vtest_vdev_class, vtest_vdev_methods, 0);

static kobj_method_t vtest_vbq_methods[] = {
	KOBJMETHOD(vbq_queue_setup,		vtest_queue_setup),
	KOBJMETHOD(vbq_buf_queue,		vtest_buf_queue),
	KOBJMETHOD(vbq_start_streaming,		vtest_start_streaming),
	KOBJMETHOD(vbq_stop_streaming,		vtest_stop_streaming),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(vtest_vbq, vtest_vbq_class, vtest_vbq_methods, 0);

/* ------------------------------------------------------------------ newbus */

static void
vtest_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, "vtest", -1) == NULL)
		BUS_ADD_CHILD(parent, 0, "vtest", 0);
}

static int
vtest_probe(device_t dev)
{

	device_set_desc(dev, "Virtual V4L2 test camera");
	return (BUS_PROBE_NOWILDCARD);
}

static int
vtest_attach(device_t dev)
{
	struct vtest_softc *sc = device_get_softc(dev);
	int error;

	sc->dev = dev;
	mtx_init(&sc->mtx, "vtest", NULL, MTX_DEF);
	callout_init_mtx(&sc->callout, &sc->mtx, 0);
	sc->width = 640;
	sc->height = 480;
	sc->pixfmt = V4L2_PIX_FMT_YUYV;
	sc->bpl = sc->width * vtest_bpp(sc->pixfmt);
	sc->sizeimage = sc->bpl * sc->height;
	sc->interval = vtest_intervals[0];

	kobj_init((kobj_t)&sc->vdev, &vtest_vdev_class);
	vbq_init(&sc->vbq, &sc->vdev, V4L2_BUF_TYPE_VIDEO_CAPTURE, &vtest_vbq_class);
	sc->vbq.min_queued = 2;
	sc->vbq.dmabuf = true;		/* frames are written through b->vaddr: imports are fine */
	sc->vbq.pages = true;		/* no DMA: do not ask for contiguous memory */

	vctrl_handler_init(&sc->ctrls);
	sc->c_bright = vctrl_new_std(&sc->ctrls, V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
	vctrl_new_std(&sc->ctrls, V4L2_CID_CONTRAST, 0, 255, 1, 128);
	vctrl_new_std(&sc->ctrls, V4L2_CID_SATURATION, 0, 255, 1, 128);
	vctrl_new_std(&sc->ctrls, V4L2_CID_HUE, -128, 127, 1, 0);
	sc->c_hflip = vctrl_new_std(&sc->ctrls, V4L2_CID_HFLIP, 0, 1, 1, 0);
	sc->c_pattern = vctrl_new_std_menu(&sc->ctrls, V4L2_CID_TEST_PATTERN,
	    nitems(vtest_patterns) - 1, 0, vtest_patterns);
	if (sc->ctrls.error != 0) {
		error = sc->ctrls.error;
		goto fail;
	}

	sc->vdev.queue = &sc->vbq;
	sc->vdev.ctrl_handler = &sc->ctrls;
	error = vdev_register(&sc->vdev, dev,
	    V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT);
	if (error != 0)
		goto fail;
	device_printf(dev, "/dev/video%d\n", sc->vdev.unit);
	return (0);
fail:
	vctrl_handler_free(&sc->ctrls);
	vbq_destroy(&sc->vbq);
	kobj_delete((kobj_t)&sc->vdev, NULL);
	mtx_destroy(&sc->mtx);
	return (error);
}

static int
vtest_detach(device_t dev)
{
	struct vtest_softc *sc = device_get_softc(dev);

	vdev_unregister(&sc->vdev);
	vbq_destroy(&sc->vbq);
	vctrl_handler_free(&sc->ctrls);
	kobj_delete((kobj_t)&sc->vdev, NULL);
	callout_drain(&sc->callout);
	mtx_destroy(&sc->mtx);
	return (0);
}

static device_method_t vtest_dev_methods[] = {
	DEVMETHOD(device_identify,	vtest_identify),
	DEVMETHOD(device_probe,		vtest_probe),
	DEVMETHOD(device_attach,	vtest_attach),
	DEVMETHOD(device_detach,	vtest_detach),
	DEVMETHOD_END
};

static driver_t vtest_driver = {
	"vtest",
	vtest_dev_methods,
	sizeof(struct vtest_softc),
};

DRIVER_MODULE(vtest, nexus, vtest_driver, 0, 0);
MODULE_DEPEND(vtest, media, 1, 1, 1);
MODULE_VERSION(vtest, 1);
