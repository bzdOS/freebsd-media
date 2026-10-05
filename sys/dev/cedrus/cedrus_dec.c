/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cedrus V4L2 decoder: a stateless MPEG-2 decoder on the A64 video engine.
 *
 * One /dev/videoN (mem2mem) and one /dev/mediaN. The OUTPUT queue takes the
 * slice data of one picture (starting at, or somewhere before, the first slice
 * start code) in V4L2_PIX_FMT_MPEG2_SLICE; the picture's headers arrive as the
 * three MPEG-2 stateless controls, set in the request of that buffer. The
 * CAPTURE queue returns the decoded frame as NV12, written by the engine.
 *
 * Pictures supported: intra (I), predicted (P) and bidirectional (B), frame
 * structure (progressive or interlaced frames), 4:2:0. Field pictures are
 * rejected. The reference pictures a request names (by the timestamp of the
 * capture buffer that holds them) are read by the engine straight from those
 * buffers. Every register,
 * field and step below names the measurement it rests on (F:Mnnn-x, see
 * docs/cedrus-facts in the bsdOS tree); a line without one is not allowed.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/cedrus/cedrus_priv.h>

extern struct bus_space memmap_bus;

/* ---------------------------------------------------------------- registers */

#define	VE_CTRL			0x000	/* F:M006-b */
#define	 VE_CTRL_MPEG		0x00130000	/* F:M006-b */
#define	VE_OUT_CHROMA_OFF	0x0c4	/* F:M010-e: irrelevant for NV12 */
#define	VE_OUT_STRIDE		0x0c8	/* F:M010-c: chroma stride / 2 << 16 | luma stride */
#define	VE_OUT_FORMAT		0x0ec	/* F:M006-b */
#define	 VE_OUT_NV12		0x40	/* F:M007-b, F:M010-c: linear NV12 */

#define	MPEG_PHDR		0x100	/* F:M010-a */
#define	MPEG_SIZE		0x108	/* F:M010-b */
#define	MPEG_FRAME_SIZE		0x10c	/* F:M010-b */
#define	MPEG_MBA		0x110	/* F:M006-d */
#define	MPEG_CTRL		0x114	/* F:M006-d */
#define	 MPEG_CTRL_JOB		0x800000b8	/* F:M006-d, interrupts on (F:M010-open) */
#define	 MPEG_CTRL_IDLE		0x80000080	/* F:M006-e */
#define	MPEG_TRIG		0x118	/* F:M006-d */
#define	 MPEG_TRIG_DECODE	0x8200000f	/* F:M006-d */
#define	MPEG_STATUS		0x11c	/* F:M006-e, F:M001 */
#define	 MPEG_STATUS_FINISH	0x1
#define	 MPEG_STATUS_ERROR	0x2
#define	 MPEG_STATUS_FLAGS	0x7	/* F:M006-e: write 1s to clear */
#define	MPEG_VLD_ADDR		0x128	/* F:M003-a */
#define	MPEG_VLD_OFFSET		0x12c	/* F:M006-d */
#define	MPEG_VLD_LEN		0x130	/* F:M006-d: bits */
#define	MPEG_VBV_END		0x134	/* F:M006-d */
#define	MPEG_REC_LUMA		0x148	/* F:M006-d */
#define	MPEG_REC_CHROMA		0x14c
#define	MPEG_FWD_LUMA		0x150
#define	MPEG_FWD_CHROMA		0x154
#define	MPEG_BACK_LUMA		0x158
#define	MPEG_BACK_CHROMA	0x15c
#define	VE_QUANT		0x180	/* F:M006-c */
#define	 VE_QUANT_INTRA		0x4000
#define	MPEG_ERR1		0x1c4	/* F:M006-d */
#define	MPEG_ERR2		0x1c8

#define	SYSCON_PA		0x01c00000	/* F:M006-a */
#define	SYSCON_VE_ENABLE	0x7fffffff

#define	CEDRUS_MIN_DIM		16
#define	CEDRUS_MAX_DIM		2048
#define	CEDRUS_MIN_STREAM	(64U << 10)
#define	CEDRUS_MAX_STREAM	(32U << 20)
#define	CEDRUS_JOB_TIMEOUT	(hz)

static MALLOC_DEFINE(M_CEDRUS_DEC, "cedrus_dec", "Cedrus V4L2 decoder");

/* --------------------------------------------------------------- structures */

struct cedrus_ctx;

struct cedrus_dec {
	struct cedrus_softc	*sc;
	struct vdev		vdev;
	struct vm2m_dev		m2m;
	struct vmedia		media;
	struct mtx		mtx;		/* the job on the hardware */
	struct resource		*irq;
	void			*ih;
	struct callout		to;
	struct task		to_task;
	struct cedrus_ctx	*cur;		/* the job on the hardware, or NULL */
	struct cedrus_ctx	*to_ctx;	/* the job that timed out */
	bool			registered;
	bool			media_registered;
	u_int			njobs, nerrors, ntimeouts, nrejects, nmissing;
};

enum { CEDRUS_OUT = 0, CEDRUS_CAP = 1 };

/* The formats of one open file: OUTPUT is set by the user, CAPTURE follows. */
struct cedrus_fs {
	uint32_t		width, height;	/* coded size, set on the OUTPUT queue */
	uint32_t		out_size;	/* OUTPUT sizeimage */
	uint32_t		colorspace, ycbcr_enc, quantization, xfer_func;
};

/* Everything one open file has. */
struct cedrus_ctx {
	struct cedrus_dec	*dec;
	struct vm2m_ctx		m2m;
	struct vbq		out, cap;
	struct vctrl_handler	ctrls;
	struct vctrl		*c_seq, *c_pic, *c_quant;
	struct cedrus_fs	fs;
};

static const struct vctrl_config cedrus_cfg_seq = {
	.id = V4L2_CID_STATELESS_MPEG2_SEQUENCE, .name = "MPEG-2 Sequence Header",
	.type = V4L2_CTRL_TYPE_MPEG2_SEQUENCE,
	.elem_size = sizeof(struct v4l2_ctrl_mpeg2_sequence),
};
static const struct vctrl_config cedrus_cfg_pic = {
	.id = V4L2_CID_STATELESS_MPEG2_PICTURE, .name = "MPEG-2 Picture Header",
	.type = V4L2_CTRL_TYPE_MPEG2_PICTURE,
	.elem_size = sizeof(struct v4l2_ctrl_mpeg2_picture),
};
static const struct vctrl_config cedrus_cfg_quant = {
	.id = V4L2_CID_STATELESS_MPEG2_QUANTISATION, .name = "MPEG-2 Quantisation Matrices",
	.type = V4L2_CTRL_TYPE_MPEG2_QUANTISATION,
	.elem_size = sizeof(struct v4l2_ctrl_mpeg2_quantisation),
};

static void cedrus_timeout(void *);

static struct cedrus_ctx *
cedrus_ctx_of(struct vfh *fh)
{

	return (fh->priv);
}

static int
cedrus_idx(uint32_t type)
{

	return (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? CEDRUS_OUT : CEDRUS_CAP);
}

/* ------------------------------------------------------------------ formats */

/* F:M010-d: a stride of 16 works; 32 keeps every plane address aligned (F:M002-b). */
static uint32_t
cedrus_stride(uint32_t width)
{

	return (roundup2(width, 32));
}

/* The engine decodes whole macroblocks (F:M010-d). */
static uint32_t
cedrus_cap_height(uint32_t height)
{

	return (roundup2(height, 16));
}

static uint32_t
cedrus_cap_size(uint32_t width, uint32_t height)
{

	return (cedrus_stride(width) * cedrus_cap_height(height) * 3 / 2);
}

static uint32_t
cedrus_clamp_dim(uint32_t v)
{

	if (v < CEDRUS_MIN_DIM)
		v = CEDRUS_MIN_DIM;
	if (v > CEDRUS_MAX_DIM)
		v = CEDRUS_MAX_DIM;
	return (v);
}

static uint32_t
cedrus_out_size(uint32_t w, uint32_t h, uint32_t want)
{
	uint32_t size = w * h * 3 / 2;

	if (size < CEDRUS_MIN_STREAM)
		size = CEDRUS_MIN_STREAM;
	if (want > size)
		size = want;
	if (size > CEDRUS_MAX_STREAM)
		size = CEDRUS_MAX_STREAM;
	return (roundup2(size, PAGE_SIZE));
}

static void
cedrus_fmt_out(const struct cedrus_fs *fs, struct v4l2_pix_format *p)
{

	memset(p, 0, sizeof(*p));
	p->width = fs->width;
	p->height = fs->height;
	p->pixelformat = V4L2_PIX_FMT_MPEG2_SLICE;
	p->field = V4L2_FIELD_NONE;
	p->sizeimage = fs->out_size;
	p->colorspace = fs->colorspace;
	p->ycbcr_enc = fs->ycbcr_enc;
	p->quantization = fs->quantization;
	p->xfer_func = fs->xfer_func;
}

/* The decoded picture is as big as the coded one, in whole macroblocks. */
static void
cedrus_fmt_cap(const struct cedrus_fs *fs, struct v4l2_pix_format *p)
{

	memset(p, 0, sizeof(*p));
	p->width = roundup2(fs->width, 16);
	p->height = cedrus_cap_height(fs->height);
	p->pixelformat = V4L2_PIX_FMT_NV12;
	p->field = V4L2_FIELD_NONE;
	p->bytesperline = cedrus_stride(fs->width);
	p->sizeimage = cedrus_cap_size(fs->width, fs->height);
	p->colorspace = fs->colorspace;
	p->ycbcr_enc = fs->ycbcr_enc;
	p->quantization = fs->quantization;
	p->xfer_func = fs->xfer_func;
}

static int
cedrus_querycap(struct vdev *v, struct vfh *fh, struct v4l2_capability *cap)
{

	strlcpy((char *)cap->driver, "cedrus", sizeof(cap->driver));
	strlcpy((char *)cap->card, "Allwinner A64 Video Engine", sizeof(cap->card));
	strlcpy((char *)cap->bus_info, "platform:cedrus", sizeof(cap->bus_info));
	return (0);
}

static int
cedrus_enum_fmt(struct vdev *v, struct vfh *fh, struct v4l2_fmtdesc *f)
{

	if (f->index != 0)
		return (EINVAL);
	if (cedrus_idx(f->type) == CEDRUS_OUT) {
		f->pixelformat = V4L2_PIX_FMT_MPEG2_SLICE;
		f->flags = V4L2_FMT_FLAG_COMPRESSED;
		strlcpy((char *)f->description, "MPEG-2 Parsed Slice Data",
		    sizeof(f->description));
	} else {
		f->pixelformat = V4L2_PIX_FMT_NV12;
		strlcpy((char *)f->description, "Y/CbCr 4:2:0",
		    sizeof(f->description));
	}
	return (0);
}

static int
cedrus_g_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_ctx_of(fh);

	if (cedrus_idx(f->type) == CEDRUS_OUT)
		cedrus_fmt_out(&ctx->fs, &f->fmt.pix);
	else
		cedrus_fmt_cap(&ctx->fs, &f->fmt.pix);
	return (0);
}

/*
 * What the device would use for *p. The OUTPUT side takes the coded size and
 * the colour description from the caller; the CAPTURE side follows it.
 */
static void
cedrus_adjust(struct cedrus_ctx *ctx, int idx, struct v4l2_pix_format *p)
{
	struct cedrus_fs tmp = ctx->fs;

	if (idx == CEDRUS_OUT) {
		tmp.width = cedrus_clamp_dim(p->width);
		tmp.height = cedrus_clamp_dim(p->height);
		tmp.out_size = cedrus_out_size(tmp.width, tmp.height, p->sizeimage);
		if (p->colorspace <= V4L2_COLORSPACE_DCI_P3)
			tmp.colorspace = p->colorspace;
		if (p->ycbcr_enc <= V4L2_YCBCR_ENC_SMPTE240M)
			tmp.ycbcr_enc = p->ycbcr_enc;
		if (p->quantization <= V4L2_QUANTIZATION_LIM_RANGE)
			tmp.quantization = p->quantization;
		if (p->xfer_func <= V4L2_XFER_FUNC_SMPTE2084)
			tmp.xfer_func = p->xfer_func;
		cedrus_fmt_out(&tmp, p);
	} else
		cedrus_fmt_cap(&tmp, p);
}

static int
cedrus_try_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{

	cedrus_adjust(cedrus_ctx_of(fh), cedrus_idx(f->type), &f->fmt.pix);
	return (0);
}

static int
cedrus_s_fmt(struct vdev *v, struct vfh *fh, struct v4l2_format *f)
{
	struct cedrus_ctx *ctx = cedrus_ctx_of(fh);
	int idx = cedrus_idx(f->type);

	cedrus_adjust(ctx, idx, &f->fmt.pix);
	if (idx == CEDRUS_CAP)
		return (0);		/* NV12 is the only choice, size follows OUTPUT */
	/* The result's size follows the source's: not while it has buffers. */
	if (vbq_has_buffers(&ctx->cap) &&
	    (f->fmt.pix.width != ctx->fs.width || f->fmt.pix.height != ctx->fs.height))
		return (EBUSY);
	ctx->fs.width = f->fmt.pix.width;
	ctx->fs.height = f->fmt.pix.height;
	ctx->fs.out_size = f->fmt.pix.sizeimage;
	ctx->fs.colorspace = f->fmt.pix.colorspace;
	ctx->fs.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->fs.quantization = f->fmt.pix.quantization;
	ctx->fs.xfer_func = f->fmt.pix.xfer_func;
	return (0);
}

static int
cedrus_enum_framesizes(struct vdev *v, struct vfh *fh, struct v4l2_frmsizeenum *f)
{

	if (f->index != 0 || (f->pixel_format != V4L2_PIX_FMT_MPEG2_SLICE &&
	    f->pixel_format != V4L2_PIX_FMT_NV12))
		return (EINVAL);
	f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	f->stepwise.min_width = f->stepwise.min_height = CEDRUS_MIN_DIM;
	f->stepwise.max_width = f->stepwise.max_height = CEDRUS_MAX_DIM;
	f->stepwise.step_width = f->stepwise.step_height = 16;
	memset(f->reserved, 0, sizeof(f->reserved));
	return (0);
}

static int
cedrus_s_ctrl(struct vdev *v, struct vfh *fh, struct vctrl *c)
{

	/* The headers are read when a job runs. */
	return (0);
}

/* -------------------------------------------------------------------- queues */

static struct cedrus_ctx *
cedrus_ctx_of_q(struct vbq *q)
{

	return (q->is_output ? __containerof(q, struct cedrus_ctx, out) :
	    __containerof(q, struct cedrus_ctx, cap));
}

static int
cedrus_queue_setup(struct vbq *q, uint32_t *nbuffers, size_t *size)
{
	struct cedrus_ctx *ctx = cedrus_ctx_of_q(q);

	if (*nbuffers < 1)
		*nbuffers = 1;
	*size = q->is_output ? ctx->fs.out_size :
	    cedrus_cap_size(ctx->fs.width, ctx->fs.height);
	return (0);
}

static void
cedrus_buf_queue(struct vbq *q, struct vbuf *b)
{

	vm2m_buf_queue(&cedrus_ctx_of_q(q)->m2m, b);
}

static void
cedrus_stop_streaming(struct vbq *q)
{

	vm2m_stop_streaming(&cedrus_ctx_of_q(q)->m2m, q);
}

static kobj_method_t cedrus_vbq_methods[] = {
	KOBJMETHOD(vbq_queue_setup,	cedrus_queue_setup),
	KOBJMETHOD(vbq_buf_queue,	cedrus_buf_queue),
	KOBJMETHOD(vbq_stop_streaming,	cedrus_stop_streaming),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(cedrus_vbq, cedrus_vbq_class, cedrus_vbq_methods, 0);

/* ----------------------------------------------------------------------- job */

static inline uint32_t
cedrus_rel(bus_addr_t pa)
{

	return ((uint32_t)(pa - DRAM_BASE));
}

/* F:M003-a: flag in 31:28, address bits 27:4, address bits 31:28 in 3:0. */
static inline uint32_t
cedrus_vld_addr(bus_addr_t pa, uint32_t flag)
{
	uint32_t a = cedrus_rel(pa);

	return ((flag << 28) | (a & 0x0ffffff0) | ((a >> 28) & 0xf));
}

/*
 * Check what the engine can do so far and find where the engine must start
 * reading. Returns 0, or an errno for a picture to reject.
 */
static int
cedrus_check(struct cedrus_ctx *ctx, const struct v4l2_ctrl_mpeg2_sequence *seq,
    const struct v4l2_ctrl_mpeg2_picture *pic, struct vbuf *src, struct vbuf *dst,
    uint32_t *startp)
{
	const uint8_t *p = src->vaddr;
	uint32_t i, n = src->bytesused;
	uint32_t w, h;

	if ((pic->picture_coding_type != V4L2_MPEG2_PIC_CODING_TYPE_I &&
	    pic->picture_coding_type != V4L2_MPEG2_PIC_CODING_TYPE_P &&
	    pic->picture_coding_type != V4L2_MPEG2_PIC_CODING_TYPE_B) ||
	    pic->picture_structure != V4L2_MPEG2_PIC_FRAME ||
	    seq->chroma_format != 1)
		return (ENOTSUP);	/* D and field pictures: not yet */
	w = seq->horizontal_size;
	h = seq->vertical_size;
	if (w < 1 || h < 1 || roundup2(w, 16) > roundup2(ctx->fs.width, 16) ||
	    cedrus_cap_height(h) > cedrus_cap_height(ctx->fs.height))
		return (EINVAL);
	if (dst->size < cedrus_cap_size(w, h) ||
	    dst->paddr < DRAM_BASE || dst->paddr + dst->size > DRAM_BASE + DMA_WINDOW ||
	    src->paddr < DRAM_BASE || src->paddr + src->size > DRAM_BASE + DMA_WINDOW)
		return (EINVAL);
	if (n > src->size)
		n = src->size;
	/* F:M010-g: the engine starts at the first slice start code. */
	for (i = 0; i + 4 <= n; i++)
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 &&
		    p[i + 3] >= 0x01 && p[i + 3] <= 0xaf)
			break;
	if (i + 4 > n)
		return (EINVAL);
	*startp = i;
	return (0);
}

/*
 * The capture buffer named by a reference timestamp. A reference that is not
 * there (a stream that starts on a P picture, a lost frame) is replaced by the
 * picture's own buffer: the result is wrong but the engine stays within memory
 * the caller owns.
 */
static struct vbuf *
cedrus_ref(struct cedrus_ctx *ctx, uint64_t ts, struct vbuf *dst)
{
	struct vbuf *b = vbq_find_timestamp(&ctx->cap, ts);

	if (b == NULL || b->paddr < DRAM_BASE ||
	    b->paddr + b->size > DRAM_BASE + DMA_WINDOW ||
	    b->size < dst->size) {
		ctx->dec->nmissing++;
		return (dst);
	}
	return (b);
}

/* F:M010-a, F:M012-a */
static uint32_t
cedrus_header_word(const struct v4l2_ctrl_mpeg2_picture *pic)
{
	uint32_t f[2][2], h;

	if (pic->picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_I) {
		/* For an intra picture the f_codes are unused and read as 15 (F:M010-a). */
		f[0][0] = f[0][1] = f[1][0] = f[1][1] = 0xf;
	} else {
		f[0][0] = pic->f_code[0][0] & 0xf;
		f[0][1] = pic->f_code[0][1] & 0xf;
		f[1][0] = pic->f_code[1][0] & 0xf;
		f[1][1] = pic->f_code[1][1] & 0xf;
	}
	h = ((uint32_t)pic->picture_coding_type << 28) |
	    (f[0][0] << 24) | (f[0][1] << 20) | (f[1][0] << 16) | (f[1][1] << 12) |
	    ((uint32_t)pic->intra_dc_precision << 10) |
	    ((uint32_t)pic->picture_structure << 8);
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST) != 0)
		h |= 1 << 7;
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT) != 0)
		h |= 1 << 6;
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV) != 0)
		h |= 1 << 5;
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE) != 0)
		h |= 1 << 4;
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_INTRA_VLC) != 0)
		h |= 1 << 3;
	if ((pic->flags & V4L2_MPEG2_PIC_FLAG_ALT_SCAN) != 0)
		h |= 1 << 2;
	return (h);
}

/* Program the engine and start the picture. d->mtx is held. */
static void
cedrus_start(struct cedrus_dec *d, struct cedrus_ctx *ctx,
    const struct v4l2_ctrl_mpeg2_sequence *seq,
    const struct v4l2_ctrl_mpeg2_picture *pic,
    const struct v4l2_ctrl_mpeg2_quantisation *quant,
    struct vbuf *src, struct vbuf *dst, struct vbuf *fwd, struct vbuf *back,
    uint32_t start)
{
	struct resource *r = d->sc->mem;
	uint32_t wm = (seq->horizontal_size + 15) / 16;
	uint32_t hm = (seq->vertical_size + 15) / 16;
	uint32_t stride = cedrus_stride(ctx->fs.width);
	uint32_t luma = cedrus_rel(dst->paddr);
	bus_addr_t coff = (bus_addr_t)stride * cedrus_cap_height(ctx->fs.height);
	uint32_t chroma = cedrus_rel(dst->paddr + coff);
	uint32_t len = src->bytesused - start;
	u_int i;

	/* F:M006-b */
	bus_write_4(r, VE_OUT_FORMAT, VE_OUT_NV12);
	bus_write_4(r, VE_OUT_CHROMA_OFF, 0);
	bus_write_4(r, VE_OUT_STRIDE, ((stride / 2) << 16) | stride);
	bus_write_4(r, VE_CTRL, VE_CTRL_MPEG);
	/* F:M006-c, F:M010-f: 4:2:0 uses the luma matrices, in stream order. */
	for (i = 0; i < 64; i++)
		bus_write_4(r, VE_QUANT, VE_QUANT_INTRA | (i << 8) |
		    quant->intra_quantiser_matrix[i]);
	for (i = 0; i < 64; i++)
		bus_write_4(r, VE_QUANT, (i << 8) | quant->non_intra_quantiser_matrix[i]);
	bus_write_4(r, MPEG_PHDR, cedrus_header_word(pic));
	bus_write_4(r, MPEG_SIZE, (wm << 8) | hm);
	/* F:M012-b: the width goes in the upper half; the engine clamps motion to it. */
	bus_write_4(r, MPEG_FRAME_SIZE, ((wm * 16) << 16) | (hm * 16));
	/* F:M006-d: all three are the picture itself for an intra picture (F:M012-c). */
	bus_write_4(r, MPEG_FWD_LUMA, cedrus_rel(fwd->paddr));
	bus_write_4(r, MPEG_FWD_CHROMA, cedrus_rel(fwd->paddr + coff));
	bus_write_4(r, MPEG_BACK_LUMA, cedrus_rel(back->paddr));
	bus_write_4(r, MPEG_BACK_CHROMA, cedrus_rel(back->paddr + coff));
	bus_write_4(r, MPEG_REC_LUMA, luma);
	bus_write_4(r, MPEG_REC_CHROMA, chroma);
	/* F:M011-a: the address has 16-byte granularity; the rest is a bit offset. */
	bus_write_4(r, MPEG_VLD_OFFSET, (uint32_t)((src->paddr + start) & 15) * 8);
	bus_write_4(r, MPEG_VLD_LEN, len * 8);
	bus_write_4(r, MPEG_VLD_ADDR, cedrus_vld_addr((src->paddr + start) & ~15UL, 7));
	bus_write_4(r, MPEG_VBV_END, cedrus_rel(src->paddr + src->bytesused));
	bus_write_4(r, MPEG_MBA, 0);
	bus_write_4(r, MPEG_ERR1, 0);
	bus_write_4(r, MPEG_ERR2, 0);
	bus_write_4(r, MPEG_CTRL, MPEG_CTRL_JOB);
	/* The buffers are uncached (M009); make sure the CPU's writes are out. */
	__asm __volatile("dsb st" ::: "memory");
	bus_write_4(r, MPEG_TRIG, MPEG_TRIG_DECODE);
}

static void
cedrus_job_done(struct cedrus_dec *d, struct cedrus_ctx *ctx, bool error)
{
	struct vbuf *dst = vm2m_next_dst(&ctx->m2m);

	if (error)
		d->nerrors++;
	else {
		d->njobs++;
		if (dst != NULL)
			dst->bytesused = dst->size;
	}
	vm2m_buf_done_and_job_finish(&ctx->m2m, error);
}

static void
cedrus_device_run(struct vm2m_dev *dev, struct vm2m_ctx *m2m)
{
	struct cedrus_ctx *ctx = __containerof(m2m, struct cedrus_ctx, m2m);
	struct cedrus_dec *d = ctx->dec;
	struct vbuf *src = vm2m_next_src(m2m), *dst = vm2m_next_dst(m2m);
	struct v4l2_ctrl_mpeg2_sequence seq;
	struct v4l2_ctrl_mpeg2_picture pic;
	struct v4l2_ctrl_mpeg2_quantisation quant;
	struct vbuf *fwd, *back;
	uint32_t start;
	int error;

	if (src == NULL || dst == NULL) {
		vm2m_buf_done_and_job_finish(m2m, true);
		return;
	}
	/* The headers set in the source's request apply to this picture. */
	vctrl_request_setup(src, &ctx->ctrls);
	if (vctrl_get_payload(ctx->c_seq, &seq, sizeof(seq)) != 0 ||
	    vctrl_get_payload(ctx->c_pic, &pic, sizeof(pic)) != 0 ||
	    vctrl_get_payload(ctx->c_quant, &quant, sizeof(quant)) != 0) {
		d->nrejects++;
		cedrus_job_done(d, ctx, true);
		return;
	}
	error = cedrus_check(ctx, &seq, &pic, src, dst, &start);
	if (error != 0) {
		if (d->nrejects++ < 5)
			device_printf(d->sc->dev, "picture rejected: %d (type %u "
			    "structure %u chroma %u %ux%u)\n", error,
			    pic.picture_coding_type, pic.picture_structure,
			    seq.chroma_format, seq.horizontal_size,
			    seq.vertical_size);
		cedrus_job_done(d, ctx, true);
		return;
	}
	/* F:M012-c: intra: the picture itself; P: forward only; B: both. */
	fwd = back = dst;
	if (pic.picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_P)
		fwd = back = cedrus_ref(ctx, pic.forward_ref_ts, dst);
	else if (pic.picture_coding_type == V4L2_MPEG2_PIC_CODING_TYPE_B) {
		fwd = cedrus_ref(ctx, pic.forward_ref_ts, dst);
		back = cedrus_ref(ctx, pic.backward_ref_ts, dst);
	}
	mtx_lock(&d->mtx);
	d->cur = ctx;
	callout_reset(&d->to, CEDRUS_JOB_TIMEOUT, cedrus_timeout, d);
	cedrus_start(d, ctx, &seq, &pic, &quant, src, dst, fwd, back, start);
	mtx_unlock(&d->mtx);
}

/* Take the job off the hardware: the one who gets it finishes it. */
static struct cedrus_ctx *
cedrus_take(struct cedrus_dec *d)
{
	struct cedrus_ctx *ctx;

	mtx_assert(&d->mtx, MA_OWNED);
	ctx = d->cur;
	d->cur = NULL;
	if (ctx != NULL)
		callout_stop(&d->to);
	return (ctx);
}

static void
cedrus_intr(void *arg)
{
	struct cedrus_dec *d = arg;
	struct resource *r = d->sc->mem;
	struct cedrus_ctx *ctx = NULL;
	uint32_t st;

	mtx_lock(&d->mtx);
	st = bus_read_4(r, MPEG_STATUS);
	if ((st & MPEG_STATUS_FLAGS) != 0) {
		/* F:M006-e: interrupts off, flags cleared, whatever they were. */
		bus_write_4(r, MPEG_CTRL, MPEG_CTRL_IDLE);
		bus_write_4(r, MPEG_STATUS, MPEG_STATUS_FLAGS);
		if ((st & (MPEG_STATUS_FINISH | MPEG_STATUS_ERROR)) != 0)
			ctx = cedrus_take(d);
	}
	mtx_unlock(&d->mtx);
	if (ctx != NULL)
		cedrus_job_done(d, ctx, (st & MPEG_STATUS_ERROR) != 0);
}

static void
cedrus_timeout(void *arg)
{
	struct cedrus_dec *d = arg;

	/* d->mtx is held (callout_init_mtx). Resetting sleeps: hand it to a task. */
	if (d->cur == NULL)
		return;
	d->to_ctx = cedrus_take(d);
	taskqueue_enqueue(taskqueue_thread, &d->to_task);
}

static void
cedrus_timeout_task(void *arg, int pending)
{
	struct cedrus_dec *d = arg;
	struct cedrus_ctx *ctx;

	mtx_lock(&d->mtx);
	ctx = d->to_ctx;
	d->to_ctx = NULL;
	mtx_unlock(&d->mtx);
	if (ctx == NULL)
		return;
	d->ntimeouts++;
	device_printf(d->sc->dev, "picture timed out, resetting the engine\n");
	cedrus_engine_reset(d->sc);
	cedrus_job_done(d, ctx, true);
}

static void
cedrus_job_abort(struct vm2m_dev *dev, struct vm2m_ctx *m2m)
{
	struct cedrus_ctx *ctx = __containerof(m2m, struct cedrus_ctx, m2m);
	struct cedrus_dec *d = ctx->dec;
	struct cedrus_ctx *cur;

	mtx_lock(&d->mtx);
	cur = (d->cur == ctx) ? cedrus_take(d) : NULL;
	mtx_unlock(&d->mtx);
	if (cur == NULL)
		return;		/* idle, or finishing by itself (interrupt, timeout) */
	cedrus_engine_reset(d->sc);
	cedrus_job_done(d, ctx, true);
}

static kobj_method_t cedrus_m2m_methods[] = {
	KOBJMETHOD(vm2m_device_run,	cedrus_device_run),
	KOBJMETHOD(vm2m_job_abort,	cedrus_job_abort),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(cedrus_m2m, cedrus_m2m_class, cedrus_m2m_methods, 0);

/* --------------------------------------------------------------- per-open state */

static int
cedrus_fh_open(struct vdev *v, struct vfh *fh)
{
	struct cedrus_dec *d = __containerof(v, struct cedrus_dec, vdev);
	struct cedrus_ctx *ctx;
	int error;

	ctx = malloc(sizeof(*ctx), M_CEDRUS_DEC, M_WAITOK | M_ZERO);
	ctx->dec = d;
	ctx->fs.width = 640;
	ctx->fs.height = 480;
	ctx->fs.out_size = cedrus_out_size(ctx->fs.width, ctx->fs.height, 0);
	ctx->fs.colorspace = V4L2_COLORSPACE_REC709;
	ctx->fs.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	ctx->fs.quantization = V4L2_QUANTIZATION_DEFAULT;
	ctx->fs.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	vbq_init(&ctx->out, v, V4L2_BUF_TYPE_VIDEO_OUTPUT, &cedrus_vbq_class);
	vbq_init(&ctx->cap, v, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cedrus_vbq_class);
	ctx->out.min_queued = ctx->cap.min_queued = 0;
	/* A stateless decoder needs the picture headers with every source buffer. */
	ctx->out.supports_requests = true;
	ctx->out.requires_requests = true;
	/* The engine reaches a window of DRAM only (F:assessment); buffers are DMA segments. */
	ctx->out.dma_maxaddr = ctx->cap.dma_maxaddr = DRAM_BASE + DMA_WINDOW - 1;
	ctx->out.ts_copy = ctx->cap.ts_copy = true;
	ctx->cap.offset_base = (off_t)1 << 30;
	vm2m_ctx_init(&ctx->m2m, &d->m2m, &ctx->out, &ctx->cap);

	vctrl_handler_init(&ctx->ctrls);
	ctx->ctrls.owner = fh;
	ctx->c_seq = vctrl_new_custom(&ctx->ctrls, &cedrus_cfg_seq);
	ctx->c_pic = vctrl_new_custom(&ctx->ctrls, &cedrus_cfg_pic);
	ctx->c_quant = vctrl_new_custom(&ctx->ctrls, &cedrus_cfg_quant);
	error = ctx->ctrls.error;
	if (error != 0) {
		vctrl_handler_free(&ctx->ctrls);
		vbq_destroy(&ctx->out);
		vbq_destroy(&ctx->cap);
		free(ctx, M_CEDRUS_DEC);
		return (error);
	}
	fh->priv = ctx;
	fh->capq = &ctx->cap;
	fh->outq = &ctx->out;
	fh->ctrl_handler = &ctx->ctrls;
	return (0);
}

static void
cedrus_fh_close(struct vdev *v, struct vfh *fh)
{
	struct cedrus_ctx *ctx = fh->priv;

	if (ctx == NULL)
		return;
	vm2m_ctx_destroy(&ctx->m2m);
	vbq_destroy(&ctx->out);
	vbq_destroy(&ctx->cap);
	vctrl_handler_free(&ctx->ctrls);
	free(ctx, M_CEDRUS_DEC);
	fh->priv = NULL;
}

static kobj_method_t cedrus_vdev_methods[] = {
	KOBJMETHOD(vdev_fh_open,		cedrus_fh_open),
	KOBJMETHOD(vdev_fh_close,		cedrus_fh_close),
	KOBJMETHOD(vdev_querycap,		cedrus_querycap),
	KOBJMETHOD(vdev_enum_fmt,		cedrus_enum_fmt),
	KOBJMETHOD(vdev_g_fmt,			cedrus_g_fmt),
	KOBJMETHOD(vdev_try_fmt,		cedrus_try_fmt),
	KOBJMETHOD(vdev_s_fmt,			cedrus_s_fmt),
	KOBJMETHOD(vdev_enum_framesizes,	cedrus_enum_framesizes),
	KOBJMETHOD(vdev_s_ctrl,			cedrus_s_ctrl),
	KOBJMETHOD_END
};
DEFINE_CLASS_0(cedrus_vdev, cedrus_vdev_class, cedrus_vdev_methods, 0);

/* ------------------------------------------------------------------- attach */

int
cedrus_dec_attach(struct cedrus_softc *sc)
{
	struct cedrus_dec *d;
	bus_space_handle_t h;
	int rid = 0, error;

	d = malloc(sizeof(*d), M_CEDRUS_DEC, M_WAITOK | M_ZERO);
	d->sc = sc;
	mtx_init(&d->mtx, "cedrus job", NULL, MTX_DEF);
	callout_init_mtx(&d->to, &d->mtx, 0);
	TASK_INIT(&d->to_task, 0, cedrus_timeout_task, d);

	/* F:M006-a: the step without which the engine does not read memory. */
	error = bus_space_map(&memmap_bus, SYSCON_PA, 0x1000, 0, &h);
	if (error != 0)
		goto fail;
	bus_space_write_4(&memmap_bus, h, 0, SYSCON_VE_ENABLE);
	bus_space_unmap(&memmap_bus, h, 0x1000);

	d->irq = bus_alloc_resource_any(sc->dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
	if (d->irq == NULL) {
		error = ENXIO;
		goto fail;
	}
	error = bus_setup_intr(sc->dev, d->irq, INTR_TYPE_MISC | INTR_MPSAFE, NULL,
	    cedrus_intr, d, &d->ih);
	if (error != 0)
		goto fail;

	kobj_init((kobj_t)&d->vdev, &cedrus_vdev_class);
	vm2m_dev_init(&d->m2m, &cedrus_m2m_class);
	vmedia_init(&d->media, "cedrus", "Allwinner A64 Video Engine",
	    "platform:cedrus", 0);
	d->vdev.mdev = &d->media;
	d->vdev.requests = true;
	d->vdev.entity_name = "cedrus-proc";
	d->vdev.entity_function = MEDIA_ENT_F_PROC_VIDEO_DECODER;
	error = vdev_register(&d->vdev, sc->dev,
	    V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING | V4L2_CAP_EXT_PIX_FORMAT);
	if (error != 0)
		goto fail;
	d->registered = true;
	error = vmedia_register(&d->media);
	if (error != 0)
		goto fail;
	d->media_registered = true;

	SYSCTL_ADD_UINT(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, "pictures",
	    CTLFLAG_RD, &d->njobs, 0, "pictures decoded");
	SYSCTL_ADD_UINT(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, "errors",
	    CTLFLAG_RD, &d->nerrors, 0, "pictures that ended in an error");
	SYSCTL_ADD_UINT(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, "timeouts",
	    CTLFLAG_RD, &d->ntimeouts, 0, "pictures that timed out");
	SYSCTL_ADD_UINT(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, "rejects",
	    CTLFLAG_RD, &d->nrejects, 0, "pictures rejected before the engine");
	SYSCTL_ADD_UINT(device_get_sysctl_ctx(sc->dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, "missing_refs",
	    CTLFLAG_RD, &d->nmissing, 0, "reference pictures not found");
	sc->dec = d;
	device_printf(sc->dev, "MPEG-2 decoder: /dev/video%d, /dev/media%d\n",
	    d->vdev.unit, d->media.unit);
	return (0);
fail:
	sc->dec = d;
	cedrus_dec_detach(sc);
	return (error);
}

void
cedrus_dec_detach(struct cedrus_softc *sc)
{
	struct cedrus_dec *d = sc->dec;

	if (d == NULL)
		return;
	if (d->registered)
		vdev_unregister(&d->vdev);
	if (d->media_registered)
		vmedia_unregister(&d->media);
	if (d->ih != NULL)
		bus_teardown_intr(sc->dev, d->irq, d->ih);
	if (d->irq != NULL)
		bus_release_resource(sc->dev, SYS_RES_IRQ, 0, d->irq);
	callout_drain(&d->to);
	taskqueue_drain(taskqueue_thread, &d->to_task);
	if (d->registered) {
		vm2m_dev_destroy(&d->m2m);
		kobj_delete((kobj_t)&d->vdev, NULL);
	}
	mtx_destroy(&d->mtx);
	free(d, M_CEDRUS_DEC);
	sc->dec = NULL;
}
