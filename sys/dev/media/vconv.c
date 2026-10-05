/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vconv: a mem2mem video converter in software, to exercise the mem2mem core.
 *
 * The OUTPUT queue takes frames in YUYV or RGB24, the CAPTURE queue returns
 * them in either format. The size is the same on both sides (no scaling), so
 * setting the source size sets the result size. Horizontal Flip and Vertical
 * Flip mirror the frame. Every open file has its own formats, queues and
 * controls.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>

#include <dev/media/media.h>
#include <dev/media/vm2m.h>

#define	VCONV_MIN_W	160
#define	VCONV_MIN_H	120
#define	VCONV_MAX_W	1920
#define	VCONV_MAX_H	1080
#define	VCONV_STEP	2
#define	VCONV_CAP_OFFSET_BASE	((off_t)1 << 30)

static MALLOC_DEFINE(M_VCONV, "vconv", "V4L2 test converter");

static const uint32_t vconv_pixfmts[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24 };

struct vconv_fmt {
	uint32_t	width, height, pixfmt, bpl, sizeimage;
	/* colour description: set by the user on the source, copied to the result */
	uint32_t	colorspace, ycbcr_enc, quantization, xfer_func;
};

enum { VCONV_OUT = 0, VCONV_CAP = 1 };

struct vconv_softc {
	device_t		dev;
	struct vdev		vdev;
	struct vm2m_dev		m2m;
};

/* Everything one open file has. */
struct vconv_ctx {
	struct vconv_softc	*sc;
	struct vm2m_ctx		m2m;
	struct vbq		out, cap;
	struct vctrl_handler	ctrls;
	struct vctrl		*c_hflip, *c_vflip;
	struct vconv_fmt	fmt[2];
};

static struct vconv_ctx *
vconv_ctx_of(struct vfh *fh)
{

	return (fh->priv);
}

static int
vconv_idx(uint32_t type)
{

	return (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? VCONV_OUT : VCONV_CAP);
}

/* ----------------------------------------------------------------- formats */

static u_int
vconv_bpp(uint32_t pixfmt)
{

	return (pixfmt == V4L2_PIX_FMT_RGB24 ? 3 : 2);
}

static bool
vconv_known(uint32_t pixfmt)
{
	u_int i;

	for (i = 0; i < nitems(vconv_pixfmts); i++)
		if (vconv_pixfmts[i] == pixfmt)
			return (true);
	return (false);
}

static uint32_t
vconv_clamp(uint32_t v, uint32_t min, uint32_t max)
{

	if (v < min)
		v = min;
	if (v > max)
		v = max;
	v = min + ((v - min + VCONV_STEP / 2) / VCONV_STEP) * VCONV_STEP;
	return (v > max ? max : v);
}

static void
vconv_set(struct vconv_fmt *f, uint32_t w, uint32_t h, uint32_t pixfmt)
{

	f->width = w;
	f->height = h;
	f->pixfmt = pixfmt;
	f->bpl = w * vconv_bpp(pixfmt);
	f->sizeimage = f->bpl * h;
}

static void
vconv_set_colour(struct vconv_fmt *f, uint32_t cs, uint32_t enc, uint32_t quant,
    uint32_t xfer)
{

	f->colorspace = cs;
	f->ycbcr_enc = enc;
	f->quantization = quant;
	f->xfer_func = xfer;
}

static void
vconv_to_v4l2(const struct vconv_fmt *f, struct v4l2_pix_format *p)
{

	memset(p, 0, sizeof(*p));
	p->width = f->width;
	p->height = f->height;
	p->pixelformat = f->pixfmt;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = f->bpl;
	p->sizeimage = f->sizeimage;
	p->colorspace = f->colorspace;
	p->ycbcr_enc = f->ycbcr_enc;
	p->quantization = f->quantization;
	p->xfer_func = f->xfer_func;
}

/*
 * What the device would use for *p on queue `idx`. The result has the source's
 * size and colour description whatever the caller asked for; the source takes
 * both from the caller (colours it does not know fall back to what it had).
 */
static void
vconv_adjust(struct vconv_ctx *ctx, int idx, struct v4l2_pix_format *p)
{
	struct vconv_fmt f;
	uint32_t w, h, pixfmt;

	pixfmt = vconv_known(p->pixelformat) ? p->pixelformat :
	    ctx->fmt[idx].pixfmt;
	if (idx == VCONV_OUT) {
		w = vconv_clamp(p->width, VCONV_MIN_W, VCONV_MAX_W);
		h = vconv_clamp(p->height, VCONV_MIN_H, VCONV_MAX_H);
	} else {
		w = ctx->fmt[VCONV_OUT].width;
		h = ctx->fmt[VCONV_OUT].height;
	}
	vconv_set(&f, w, h, pixfmt);
	if (idx == VCONV_OUT) {
		const struct vconv_fmt *old = &ctx->fmt[VCONV_OUT];

		vconv_set_colour(&f,
		    p->colorspace <= V4L2_COLORSPACE_DCI_P3 ? p->colorspace : old->colorspace,
		    p->ycbcr_enc <= V4L2_YCBCR_ENC_SMPTE240M ? p->ycbcr_enc : old->ycbcr_enc,
		    p->quantization <= V4L2_QUANTIZATION_LIM_RANGE ? p->quantization : old->quantization,
		    p->xfer_func <= V4L2_XFER_FUNC_SMPTE2084 ? p->xfer_func : old->xfer_func);
	} else {
		const struct vconv_fmt *src = &ctx->fmt[VCONV_OUT];

		vconv_set_colour(&f, src->colorspace, src->ycbcr_enc,
		    src->quantization, src->xfer_func);
	}
	vconv_to_v4l2(&f, p);
}

static int
vconv_querycap(struct vdev *v, struct vfh *fh, struct v4l2_capability *cap)
{

	strlcpy((char *)cap->driver, "vconv", sizeof(cap->driver));
	strlcpy((char *)cap->card, "Virtual Test Converter", sizeof(cap->card));
	strlcpy((char *)cap->bus_info, "platform:vconv", sizeof(cap->bus_info));
	return (0);
}

static int
vconv_enum_fmt(struct vdev *v, struct vfh *fh, struct v4l2_fmtdesc *f)
{

	if (f->index >= nitems(vconv_pixfmts))
		return (EINVAL);
	f->pixelformat = vconv_pixfmts[f->index];
	strlcpy((char *)f->description,
	    f->pixelformat == V4L2_PIX_FMT_RGB24 ? "24-bit RGB 8-8-8" :
	    "YUYV 4:2:2", sizeof(f->description));
	return (0);
}

static int
vconv_g_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct vconv_ctx *ctx = vconv_ctx_of(fh);

	vconv_to_v4l2(&ctx->fmt[vconv_idx(f->type)], &f->fmt.pix);
	return (0);
}

static int
vconv_try_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{

	vconv_adjust(vconv_ctx_of(fh), vconv_idx(f->type), &f->fmt.pix);
	return (0);
}

static int
vconv_s_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct vconv_ctx *ctx = vconv_ctx_of(fh);
	int idx = vconv_idx(f->type);

	/* The two sizes are tied: changing the source changes the result. */
	if (idx == VCONV_OUT && vbq_has_buffers(&ctx->cap))
		return (EBUSY);
	vconv_adjust(ctx, idx, &f->fmt.pix);
	vconv_set(&ctx->fmt[idx], f->fmt.pix.width, f->fmt.pix.height,
	    f->fmt.pix.pixelformat);
	vconv_set_colour(&ctx->fmt[idx], f->fmt.pix.colorspace,
	    f->fmt.pix.ycbcr_enc, f->fmt.pix.quantization, f->fmt.pix.xfer_func);
	if (idx == VCONV_OUT) {
		/* The result follows the source: size and colours. */
		vconv_set(&ctx->fmt[VCONV_CAP], f->fmt.pix.width,
		    f->fmt.pix.height, ctx->fmt[VCONV_CAP].pixfmt);
		vconv_set_colour(&ctx->fmt[VCONV_CAP], f->fmt.pix.colorspace,
		    f->fmt.pix.ycbcr_enc, f->fmt.pix.quantization,
		    f->fmt.pix.xfer_func);
	}
	return (0);
}

static int
vconv_enum_framesizes(struct vdev *v, struct vfh *fh, struct v4l2_frmsizeenum *f)
{

	if (f->index != 0 || !vconv_known(f->pixel_format))
		return (EINVAL);
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = VCONV_MIN_W;
	f->stepwise.max_width = VCONV_MAX_W;
	f->stepwise.step_width = VCONV_STEP;
	f->stepwise.min_height = VCONV_MIN_H;
	f->stepwise.max_height = VCONV_MAX_H;
	f->stepwise.step_height = VCONV_STEP;
	memset(f->reserved, 0, sizeof(f->reserved));
	return (0);
}

static int
vconv_s_ctrl(struct vdev *v, struct vfh *fh, struct vctrl *c)
{

	/* The flips are read when a job runs. */
	return (0);
}

/* -------------------------------------------------------------- conversion */

struct rgb { uint8_t r, g, b; };

static inline uint8_t
vconv_clip(int v)
{

	return (v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Pixel x of a source row (BT.601, limited range for the YUV side). */
static inline struct rgb
vconv_get(uint32_t pixfmt, const uint8_t *row, u_int x)
{
	struct rgb p;

	if (pixfmt == V4L2_PIX_FMT_RGB24) {
		p.r = row[3 * x];
		p.g = row[3 * x + 1];
		p.b = row[3 * x + 2];
	} else {
		const uint8_t *q = row + (x & ~1u) * 2;
		int c = 298 * ((int)q[(x & 1) * 2] - 16);
		int d = (int)q[1] - 128, e = (int)q[3] - 128;

		p.r = vconv_clip((c + 409 * e + 128) >> 8);
		p.g = vconv_clip((c - 100 * d - 208 * e + 128) >> 8);
		p.b = vconv_clip((c + 516 * d + 128) >> 8);
	}
	return (p);
}

static void
vconv_convert(const struct vconv_fmt *sf, const uint8_t *src,
    const struct vconv_fmt *df, uint8_t *dst, bool hflip, bool vflip)
{
	u_int w = df->width, h = df->height, x, y;

	for (y = 0; y < h; y++) {
		const uint8_t *srow = src + (size_t)(vflip ? h - 1 - y : y) * sf->bpl;
		uint8_t *drow = dst + (size_t)y * df->bpl;

		for (x = 0; x < w; x += 2) {
			struct rgb a = vconv_get(sf->pixfmt, srow, hflip ? w - 1 - x : x);
			struct rgb b = vconv_get(sf->pixfmt, srow, hflip ? w - 2 - x : x + 1);

			if (df->pixfmt == V4L2_PIX_FMT_RGB24) {
				drow[3 * x + 0] = a.r;
				drow[3 * x + 1] = a.g;
				drow[3 * x + 2] = a.b;
				drow[3 * x + 3] = b.r;
				drow[3 * x + 4] = b.g;
				drow[3 * x + 5] = b.b;
			} else {
				int r = (a.r + b.r) / 2, g = (a.g + b.g) / 2,
				    bl = (a.b + b.b) / 2;

				drow[2 * x + 0] = ((66 * a.r + 129 * a.g + 25 * a.b + 128) >> 8) + 16;
				drow[2 * x + 1] = ((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128;
				drow[2 * x + 2] = ((66 * b.r + 129 * b.g + 25 * b.b + 128) >> 8) + 16;
				drow[2 * x + 3] = ((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128;
			}
		}
	}
}

static void
vconv_device_run(struct vm2m_dev *dev, struct vm2m_ctx *m2m)
{
	struct vconv_ctx *ctx = __containerof(m2m, struct vconv_ctx, m2m);
	struct vbuf *src = vm2m_next_src(m2m), *dst = vm2m_next_dst(m2m);
	bool error = false;

	if (src == NULL || dst == NULL) {
		vm2m_buf_done_and_job_finish(m2m, true);
		return;
	}
	if (src->bytesused < ctx->fmt[VCONV_OUT].sizeimage) {
		error = true;		/* a short source frame is a bad frame */
	} else {
		vconv_convert(&ctx->fmt[VCONV_OUT], src->vaddr,
		    &ctx->fmt[VCONV_CAP], dst->vaddr,
		    vctrl_get(ctx->c_hflip) != 0, vctrl_get(ctx->c_vflip) != 0);
		dst->bytesused = ctx->fmt[VCONV_CAP].sizeimage;
	}
	vm2m_buf_done_and_job_finish(m2m, error);
}

/* ------------------------------------------------------------------ queues */

static struct vconv_ctx *
vconv_ctx_of_q(struct vbq *q)
{

	return (q->is_output ? __containerof(q, struct vconv_ctx, out) :
	    __containerof(q, struct vconv_ctx, cap));
}

static int
vconv_queue_setup(struct vbq *q, uint32_t *nbuffers, size_t *size)
{
	struct vconv_ctx *ctx = vconv_ctx_of_q(q);

	if (*nbuffers < 1)
		*nbuffers = 1;
	*size = ctx->fmt[vconv_idx(q->type)].sizeimage;
	return (0);
}

static void
vconv_buf_queue(struct vbq *q, struct vbuf *b)
{

	vm2m_buf_queue(&vconv_ctx_of_q(q)->m2m, b);
}

static void
vconv_stop_streaming(struct vbq *q)
{

	vm2m_stop_streaming(&vconv_ctx_of_q(q)->m2m, q);
}

/* ------------------------------------------------------------- per-open state */

static kobj_method_t vconv_vbq_methods[] = {
	KOBJMETHOD(vbq_queue_setup,	vconv_queue_setup),
	KOBJMETHOD(vbq_buf_queue,	vconv_buf_queue),
	KOBJMETHOD(vbq_stop_streaming,	vconv_stop_streaming),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(vconv_vbq, vconv_vbq_class, vconv_vbq_methods, 0);

static int
vconv_fh_open(struct vdev *v, struct vfh *fh)
{
	struct vconv_softc *sc = __containerof(v, struct vconv_softc, vdev);
	struct vconv_ctx *ctx;

	ctx = malloc(sizeof(*ctx), M_VCONV, M_WAITOK | M_ZERO);
	ctx->sc = sc;
	vconv_set(&ctx->fmt[VCONV_OUT], 640, 480, V4L2_PIX_FMT_YUYV);
	vconv_set(&ctx->fmt[VCONV_CAP], 640, 480, V4L2_PIX_FMT_RGB24);
	vconv_set_colour(&ctx->fmt[VCONV_OUT], V4L2_COLORSPACE_SRGB,
	    V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT, V4L2_XFER_FUNC_DEFAULT);
	vconv_set_colour(&ctx->fmt[VCONV_CAP], V4L2_COLORSPACE_SRGB,
	    V4L2_YCBCR_ENC_DEFAULT, V4L2_QUANTIZATION_DEFAULT, V4L2_XFER_FUNC_DEFAULT);

	vbq_init(&ctx->out, v, V4L2_BUF_TYPE_VIDEO_OUTPUT, &vconv_vbq_class);
	vbq_init(&ctx->cap, v, V4L2_BUF_TYPE_VIDEO_CAPTURE, &vconv_vbq_class);
	ctx->out.min_queued = ctx->cap.min_queued = 0;
	ctx->out.pages = ctx->cap.pages = true;
	ctx->out.dmabuf = ctx->cap.dmabuf = true;	/* only b->vaddr is used */
	ctx->out.ts_copy = ctx->cap.ts_copy = true;
	ctx->cap.offset_base = VCONV_CAP_OFFSET_BASE;
	vm2m_ctx_init(&ctx->m2m, &sc->m2m, &ctx->out, &ctx->cap);

	vctrl_handler_init(&ctx->ctrls);
	ctx->ctrls.owner = fh;
	ctx->c_hflip = vctrl_new_std(&ctx->ctrls, V4L2_CID_HFLIP, 0, 1, 1, 0);
	ctx->c_vflip = vctrl_new_std(&ctx->ctrls, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (ctx->ctrls.error != 0) {
		int error = ctx->ctrls.error;

		vctrl_handler_free(&ctx->ctrls);
		vbq_destroy(&ctx->out);
		vbq_destroy(&ctx->cap);
		free(ctx, M_VCONV);
		return (error);
	}
	fh->priv = ctx;
	fh->capq = &ctx->cap;
	fh->outq = &ctx->out;
	fh->ctrl_handler = &ctx->ctrls;
	return (0);
}

static void
vconv_fh_close(struct vdev *v, struct vfh *fh)
{
	struct vconv_ctx *ctx = fh->priv;

	if (ctx == NULL)
		return;
	vm2m_ctx_destroy(&ctx->m2m);
	vbq_destroy(&ctx->out);
	vbq_destroy(&ctx->cap);
	vctrl_handler_free(&ctx->ctrls);
	free(ctx, M_VCONV);
	fh->priv = NULL;
}

static kobj_method_t vconv_vdev_methods[] = {
	KOBJMETHOD(vdev_fh_open,		vconv_fh_open),
	KOBJMETHOD(vdev_fh_close,		vconv_fh_close),
	KOBJMETHOD(vdev_querycap,		vconv_querycap),
	KOBJMETHOD(vdev_enum_fmt,		vconv_enum_fmt),
	KOBJMETHOD(vdev_g_fmt,			vconv_g_fmt),
	KOBJMETHOD(vdev_try_fmt,		vconv_try_fmt),
	KOBJMETHOD(vdev_s_fmt,			vconv_s_fmt),
	KOBJMETHOD(vdev_enum_framesizes,	vconv_enum_framesizes),
	KOBJMETHOD(vdev_s_ctrl,			vconv_s_ctrl),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(vconv_vdev, vconv_vdev_class, vconv_vdev_methods, 0);

static kobj_method_t vconv_m2m_methods[] = {
	KOBJMETHOD(vm2m_device_run,	vconv_device_run),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(vconv_m2m, vconv_m2m_class, vconv_m2m_methods, 0);

/* ------------------------------------------------------------------ newbus */

static void
vconv_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, "vconv", -1) == NULL)
		BUS_ADD_CHILD(parent, 0, "vconv", 0);
}

static int
vconv_probe(device_t dev)
{

	device_set_desc(dev, "Virtual V4L2 test converter");
	return (BUS_PROBE_NOWILDCARD);
}

static int
vconv_attach(device_t dev)
{
	struct vconv_softc *sc = device_get_softc(dev);
	int error;

	sc->dev = dev;
	kobj_init((kobj_t)&sc->vdev, &vconv_vdev_class);
	vm2m_dev_init(&sc->m2m, &vconv_m2m_class);
	error = vdev_register(&sc->vdev, dev,
	    V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT);
	if (error != 0) {
		vm2m_dev_destroy(&sc->m2m);
		kobj_delete((kobj_t)&sc->vdev, NULL);
		return (error);
	}
	device_printf(dev, "/dev/video%d\n", sc->vdev.unit);
	return (0);
}

static int
vconv_detach(device_t dev)
{
	struct vconv_softc *sc = device_get_softc(dev);

	vdev_unregister(&sc->vdev);
	vm2m_dev_destroy(&sc->m2m);
	kobj_delete((kobj_t)&sc->vdev, NULL);
	return (0);
}

static device_method_t vconv_dev_methods[] = {
	DEVMETHOD(device_identify,	vconv_identify),
	DEVMETHOD(device_probe,		vconv_probe),
	DEVMETHOD(device_attach,	vconv_attach),
	DEVMETHOD(device_detach,	vconv_detach),
	DEVMETHOD_END
};

static driver_t vconv_driver = {
	"vconv",
	vconv_dev_methods,
	sizeof(struct vconv_softc),
};

DRIVER_MODULE(vconv, nexus, vconv_driver, 0, 0);
MODULE_DEPEND(vconv, media, 1, 1, 1);
MODULE_VERSION(vconv, 1);
