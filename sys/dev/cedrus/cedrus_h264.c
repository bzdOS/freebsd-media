/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cedrus V4L2 decoder, H.264 part: a stateless decoder, slice based, Annex B start
 * codes. Every request carries one slice: the controls (SPS, PPS, scaling matrices,
 * prediction weights, slice parameters, decode parameters) and the slice data of the
 * OUTPUT buffer, starting at its start code. Slices of one picture go to one capture
 * buffer (the OUTPUT buffers but the last carry V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF).
 * The engine parses no slice header: this code programs it from the controls, as
 * measured in docs/cedrus-facts (M014; F:M014-x names the fact each step rests on).
 *
 * Supported: frame pictures (no field or MBAFF coding), 4:2:0, 8 bit, no slice groups,
 * I, P and B slices. Anything else is refused and the buffers come back flagged ERROR.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <machine/bus.h>
#include <machine/cpufunc.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>

#include <dev/cedrus/cedrus_dec.h>

#define	CEDRUS_H264_SLICES	16
#define	CEDRUS_H264_ENTRIES	18	/* picture table entries, 8 words each (F:M014-e) */

struct cedrus_h264 {
	struct v4l2_ctrl_h264_sps		sps;
	struct v4l2_ctrl_h264_pps		pps;
	struct v4l2_ctrl_h264_scaling_matrix	sm;
	struct v4l2_ctrl_h264_pred_weights	pw;
	struct v4l2_ctrl_h264_decode_params	dp;
	struct v4l2_ctrl_h264_slice_params	sp[CEDRUS_H264_SLICES];
	struct cedrus_mem			scratch[2];
	struct cedrus_mem			mv[VBQ_MAX_BUFFERS];
	uint32_t				geom;		/* what the buffers were made for */
	uint32_t				cur_slot;
};

/* ------------------------------------------------------------------ memory */

u_int cedrus_aux_mb = 64;	/* the engine's own buffers: how many MiB from DRAM_BASE (experiments) */
static vm_paddr_t cedrus_dst_pa;	/* the destination picture of the slice being prepared */

int
cedrus_mem_alloc(struct cedrus_mem *m, size_t size)
{
	size_t o;
	u_long hi, max;

	size = round_page(size);
	/*
	 * M_NOWAIT cannot wait for defragmentation, so a narrow window fails
	 * intermittently once the V4L2 buffers have fragmented it: measured with
	 * f3.264, 3 pictures, ten runs each — 5/10 rejected slices at a 64 MiB
	 * window, 1/10 at 256, 0/10 at 128 and 512.  The ladder therefore has to go
	 * well past the data buffers' own window as well: at 1280x720 the two
	 * scratch buffers are 3.5 MiB each (F:M017-d) and a 256 MiB window could
	 * not place them at all, while 512 MiB and 1 GiB both decoded the stream
	 * bit-exactly (F:M018-b).  The engine addresses the whole of DRAM, so a
	 * wider bound is safe.
	 */
	hi = (u_long)cedrus_aux_mb << 20;
	max = MAX((u_long)cedrus_dma_mb, 1024UL) << 20;
	if (max < hi)
		max = hi;
	/*
	 * Experiments (F:M016): the lab's buffers always sit in one 64 MiB segment,
	 * while the driver's never do — its own `mixed` counter is set on every
	 * slice, the motion vector and scratch buffers come from the low window and
	 * the pictures land high.  With this knob the engine's own buffers are taken
	 * from the destination picture's own 64 MiB segment first, which is the
	 * arrangement the lab has and the window tests never changed.
	 */
	if (cedrus_aux_near != 0 && cedrus_dst_pa > DRAM_BASE) {
		u_long seg = (u_long)(cedrus_dst_pa - DRAM_BASE) & ~((64UL << 20) - 1);

		m->kva = contigmalloc(size, M_CEDRUS, M_NOWAIT | M_ZERO,
		    DRAM_BASE + seg, DRAM_BASE + seg + (64UL << 20) - 1, PAGE_SIZE, 0);
		if (m->kva != NULL)
			goto got;
	}
	for (;;) {
		m->kva = contigmalloc(size, M_CEDRUS, M_NOWAIT | M_ZERO, DRAM_BASE,
		    DRAM_BASE + hi - 1, PAGE_SIZE, 0);
		if (m->kva != NULL)
			break;
		if (hi >= max)
			return (ENOMEM);
		hi = MIN(hi << 2, max);
	}
got:
	/* The engine reads and writes this behind the CPU's back (M009). */
	cpu_dcache_wbinv_range(m->kva, size);
	for (o = 0; o < size; o += PAGE_SIZE)
		pmap_page_set_memattr(PHYS_TO_VM_PAGE(pmap_kextract((vm_offset_t)m->kva + o)),
		    VM_MEMATTR_UNCACHEABLE);
	(void)pmap_change_attr((vm_offset_t)m->kva, size, VM_MEMATTR_UNCACHEABLE);
	m->pa = pmap_kextract((vm_offset_t)m->kva);
	m->size = size;
	return (0);
}

void
cedrus_mem_free(struct cedrus_mem *m)
{
	size_t o;

	if (m->kva == NULL)
		return;
	for (o = 0; o < m->size; o += PAGE_SIZE)
		pmap_page_set_memattr(PHYS_TO_VM_PAGE(pmap_kextract((vm_offset_t)m->kva + o)),
		    VM_MEMATTR_WRITE_BACK);
	(void)pmap_change_attr((vm_offset_t)m->kva, m->size, VM_MEMATTR_WRITE_BACK);
	contigfree(m->kva, m->size, M_CEDRUS);
	m->kva = NULL;
	m->size = 0;
}

/* --------------------------------------------------------------------- controls */

static const char * const cedrus_h264_mode_names[] = { "Slice-Based", "Frame-Based" };
static const char * const cedrus_h264_start_names[] = { "No Start Code", "Annex B Start Code" };

static const struct vctrl_config cfg_sps = {
	.id = V4L2_CID_STATELESS_H264_SPS, .name = "H264 Sequence Parameter Set",
	.type = V4L2_CTRL_TYPE_H264_SPS, .elem_size = sizeof(struct v4l2_ctrl_h264_sps),
};
static const struct vctrl_config cfg_pps = {
	.id = V4L2_CID_STATELESS_H264_PPS, .name = "H264 Picture Parameter Set",
	.type = V4L2_CTRL_TYPE_H264_PPS, .elem_size = sizeof(struct v4l2_ctrl_h264_pps),
};
static const struct vctrl_config cfg_sm = {
	.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX, .name = "H264 Scaling Matrix",
	.type = V4L2_CTRL_TYPE_H264_SCALING_MATRIX,
	.elem_size = sizeof(struct v4l2_ctrl_h264_scaling_matrix),
};
static const struct vctrl_config cfg_pw = {
	.id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS, .name = "H264 Prediction Weight Table",
	.type = V4L2_CTRL_TYPE_H264_PRED_WEIGHTS,
	.elem_size = sizeof(struct v4l2_ctrl_h264_pred_weights),
};
static const struct vctrl_config cfg_sp = {
	.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS, .name = "H264 Slice Parameters",
	.type = V4L2_CTRL_TYPE_H264_SLICE_PARAMS,
	.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
	.elem_size = sizeof(struct v4l2_ctrl_h264_slice_params),
	.nr_dims = 1, .dims = { CEDRUS_H264_SLICES },
};
static const struct vctrl_config cfg_dp = {
	.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .name = "H264 Decode Parameters",
	.type = V4L2_CTRL_TYPE_H264_DECODE_PARAMS,
	.elem_size = sizeof(struct v4l2_ctrl_h264_decode_params),
};

int
cedrus_h264_controls(struct cedrus_ctx *ctx)
{
	struct vctrl_handler *h = &ctx->ctrls;

	ctx->h264 = malloc(sizeof(*ctx->h264), M_CEDRUS, M_WAITOK | M_ZERO);
	/* Slice based decoding with Annex B start codes: all this driver does. */
	ctx->c_h264[0] = vctrl_new_std_menu_range(h, V4L2_CID_STATELESS_H264_DECODE_MODE,
	    V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	    V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	    V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED, cedrus_h264_mode_names);
	ctx->c_h264[1] = vctrl_new_std_menu_range(h, V4L2_CID_STATELESS_H264_START_CODE,
	    V4L2_STATELESS_H264_START_CODE_ANNEX_B, V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	    V4L2_STATELESS_H264_START_CODE_ANNEX_B, cedrus_h264_start_names);
	ctx->c_h264[2] = vctrl_new_custom(h, &cfg_sps);
	ctx->c_h264[3] = vctrl_new_custom(h, &cfg_pps);
	ctx->c_h264[4] = vctrl_new_custom(h, &cfg_sm);
	ctx->c_h264[5] = vctrl_new_custom(h, &cfg_pw);
	ctx->c_h264[6] = vctrl_new_custom(h, &cfg_sp);
	ctx->c_h264[7] = vctrl_new_custom(h, &cfg_dp);
	return (h->error);
}

void
cedrus_h264_free(struct cedrus_ctx *ctx)
{
	struct cedrus_h264 *h = ctx->h264;
	u_int i;

	if (h == NULL)
		return;
	for (i = 0; i < nitems(h->mv); i++)
		cedrus_mem_free(&h->mv[i]);
	cedrus_mem_free(&h->scratch[0]);
	cedrus_mem_free(&h->scratch[1]);
	free(h, M_CEDRUS);
	ctx->h264 = NULL;
}

/* -------------------------------------------------------------------- the slice */

static inline uint32_t
rd(struct cedrus_dec *d, u_int o)
{

	return (bus_read_4(d->sc->mem, o));
}

/* The register writes of the last slice, for finding out what differs from a measurement. */
#define	CEDRUS_TRACE_MAX	8192
static uint32_t cedrus_tr[CEDRUS_TRACE_MAX][2];
static u_int cedrus_ntr;
static uint32_t cedrus_sum[3];	/* FNV of the slice data: at job start, at trigger, at completion */
static void *cedrus_sumva;
static size_t cedrus_sumlen;
static uint32_t
cedrus_fnv(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t h = 2166136261U;

	while (n-- > 0)
		h = (h ^ *b++) * 16777619U;
	return (h);
}
u_int cedrus_h264_zero;	/* experiments: 1 zero after the slice data, 2 zero the picture at its first slice, 4 zero scratch+mv */
u_int cedrus_mv_min_kb;	/* experiments: minimum size of a motion vector buffer */
u_int cedrus_mv_kb;	/* experiments: force the per-picture MV buffer size (KiB, 0 = formula) */
u_int cedrus_scratch_kb;	/* experiments: force the scratch buffer size (KiB, 0 = formula) */
u_int cedrus_aux_near;	/* experiments: take the engine buffers from the picture's 64 MiB segment */
u_int cedrus_h264_flush;	/* experiments: write back + invalidate the cacheable direct-map alias of the engine's buffers */
static u_int cedrus_nsl, cedrus_nmix;	/* slices started; slices whose buffers are not in one 64 MiB segment */
static uint32_t cedrus_last[6];	/* src, luma, mv, scratch a, scratch b, first mb of the last slice */
static uint32_t cedrus_refsum[4];	/* experiments: FNV of the first two references (luma, mv) */
static u_int cedrus_nref;		/* how many references the last slice had */
/*
 * Experiments: FNV of the visible picture the engine wrote, read through the
 * uncached direct map, in the order the slices completed.  If the engine's
 * output matches the reference here while the user mapping does not, the fault
 * is the media core's buffer coherency and not the engine.
 */
static uint32_t cedrus_dstsum[32];
static u_int cedrus_dstn;
static u_int cedrus_dst_stride, cedrus_dst_w, cedrus_dst_h, cedrus_dst_ha;

static uint32_t
cedrus_fnv_frame(vm_paddr_t pa, u_int stride, u_int w, u_int h, u_int ha)
{
	const uint8_t *p = (const uint8_t *)PHYS_TO_DMAP(pa);
	uint32_t v = 2166136261U;
	u_int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			v = (v ^ p[(size_t)y * stride + x]) * 16777619U;
	for (y = 0; y < h / 2; y++)
		for (x = 0; x < w / 2; x++) {
			v = (v ^ p[(size_t)ha * stride + (size_t)y * stride + 2 * x]) * 16777619U;
			v = (v ^ p[(size_t)ha * stride + (size_t)y * stride + 2 * x + 1]) * 16777619U;
		}
	return (v);
}
u_int cedrus_h264_post_delay;		/* microseconds after the finish interrupt (experiments) */
u_int cedrus_h264_poll;			/* complete by polling instead of the interrupt (experiments) */
u_int cedrus_h264_reset;			/* reset the engine before every slice (experiments) */
u_int cedrus_h264_delay;		/* microseconds before programming a slice (experiments) */
u_int cedrus_h264_ack;			/* experiments: 0 write back the status read, 1 that | 0x7, 2 just 0x7 */
u_int cedrus_h264_refsum;		/* experiments: read the reference buffers from the CPU before a slice */

static inline void
wr(struct cedrus_dec *d, u_int o, uint32_t v)
{

	if (cedrus_ntr < CEDRUS_TRACE_MAX) {
		cedrus_tr[cedrus_ntr][0] = o;
		cedrus_tr[cedrus_ntr][1] = v;
		cedrus_ntr++;
	}
	bus_write_4(d->sc->mem, o, v);
}

/* sysctl dev.cedrus.0.h264_last: addresses of the last slice (experiments). */
int
cedrus_h264_last_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[220];
	vm_memattr_t sa = VM_MEMATTR_DEFAULT, da = VM_MEMATTR_DEFAULT;
	vm_page_t m;

	/* experiments: the attribute the media core's buffers actually carry */
	m = PHYS_TO_VM_PAGE(DRAM_BASE + cedrus_last[0]);
	if (m != NULL)
		sa = pmap_page_get_memattr(m);
	m = PHYS_TO_VM_PAGE(DRAM_BASE + cedrus_last[1]);
	if (m != NULL)
		da = pmap_page_get_memattr(m);
	snprintf(buf, sizeof(buf), "slices %u mixed %u src %08x luma %08x mv %08x scr %08x/%08x mb %u srcattr %d dstattr %d", cedrus_nsl, cedrus_nmix, cedrus_last[0],
	    cedrus_last[1], cedrus_last[2], cedrus_last[3], cedrus_last[4], cedrus_last[5], (int)sa, (int)da);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* sysctl dev.cedrus.0.h264_dst: FNV of the frames the engine wrote, in order (experiments). */
int
cedrus_h264_dst_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[256];
	u_int i;
	int n = 0;

	if (req->newptr != NULL) {		/* write to reset the list */
		cedrus_dstn = 0;
		return (0);
	}
	n += snprintf(buf + n, sizeof(buf) - n, "frames %u", cedrus_dstn);
	for (i = 0; i < cedrus_dstn && i < 12 && n < (int)sizeof(buf) - 12; i++)
		n += snprintf(buf + n, sizeof(buf) - n, " %08x", cedrus_dstsum[i]);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* sysctl dev.cedrus.0.h264_ref: FNV of what the engine is about to read (experiments). */
int
cedrus_h264_ref_sysctl(SYSCTL_HANDLER_ARGS)
{
	char buf[160];

	snprintf(buf, sizeof(buf), "refs %u luma0 %08x mv0 %08x luma1 %08x mv1 %08x",
	    cedrus_nref, cedrus_refsum[0], cedrus_refsum[1], cedrus_refsum[2], cedrus_refsum[3]);
	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/* sysctl dev.cedrus.0.h264_trace: print what the last H.264 slice wrote. */
int
cedrus_h264_trace_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct cedrus_softc *sc = arg1;
	int v = 0, error;
	u_int i;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL || v == 0)
		return (error);
	for (i = 0; i < cedrus_ntr; i++)
		device_printf(sc->dev, "W %03x %08x\n", cedrus_tr[i][0], cedrus_tr[i][1]);
	cedrus_ntr = 0;
	return (0);
}

static void
sram(struct cedrus_dec *d, u_int base, const uint32_t *w, u_int n)
{
	u_int i;

	wr(d, VE_SRAM_PTR, base);
	for (i = 0; i < n; i++)
		wr(d, VE_SRAM_DATA, w[i]);
}

/* Bytes packed little endian into words (F:M014-f, F:M014-h). */
static void
pack(uint32_t *w, const uint8_t *b, u_int n)
{
	u_int i;

	for (i = 0; i < (n + 3) / 4; i++)
		w[i] = 0;
	for (i = 0; i < n; i++)
		w[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
}

/* The motion vector buffer of capture buffer `idx` (F:M014-k), made when first needed. */
static int
cedrus_h264_mv(struct cedrus_h264 *h, uint32_t idx, uint32_t wmb, uint32_t hmb)
{
	size_t need = MAX((size_t)wmb * (hmb + 1) * 32 + 1024, (size_t)cedrus_mv_min_kb << 10);

	if (cedrus_mv_kb != 0)
		need = (size_t)cedrus_mv_kb << 10;	/* experiments: bisect the real need */

	if (idx >= nitems(h->mv))
		return (EINVAL);
	if (h->mv[idx].kva != NULL && h->mv[idx].size >= need)
		return (0);
	cedrus_mem_free(&h->mv[idx]);
	return (cedrus_mem_alloc(&h->mv[idx], need));
}

int
cedrus_h264_prepare(struct cedrus_dec *d, struct cedrus_ctx *ctx, struct vbuf *src,
    struct vbuf *dst)
{
	struct cedrus_h264 *h = ctx->h264;
	struct v4l2_ctrl_h264_sps *sps = &h->sps;
	struct v4l2_ctrl_h264_pps *pps = &h->pps;
	struct v4l2_ctrl_h264_slice_params *sp = &h->sp[0];
	struct v4l2_ctrl_h264_decode_params *dp = &h->dp;
	uint32_t wmb, hmb, geom, i, slot, stride;
	size_t scratch;
	int error;

	if (h == NULL)
		return (EINVAL);
	if (vctrl_get_payload(ctx->c_h264[2], sps, sizeof(*sps)) != 0 ||
	    vctrl_get_payload(ctx->c_h264[3], pps, sizeof(*pps)) != 0 ||
	    vctrl_get_payload(ctx->c_h264[4], &h->sm, sizeof(h->sm)) != 0 ||
	    vctrl_get_payload(ctx->c_h264[5], &h->pw, sizeof(h->pw)) != 0 ||
	    vctrl_get_payload(ctx->c_h264[6], h->sp, sizeof(h->sp)) != 0 ||
	    vctrl_get_payload(ctx->c_h264[7], dp, sizeof(*dp)) != 0)
		return (EINVAL);
	if (ctx->c_h264[6]->elems != 1)
		return (ENOTSUP);	/* slice based: one slice per request */
	/* What the engine cannot do (F:M014 scope). */
	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 != 0 ||
	    sps->bit_depth_chroma_minus8 != 0 ||
	    (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE) != 0 ||
	    (sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) == 0 ||
	    (sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) != 0 ||
	    pps->num_slice_groups_minus1 != 0 ||
	    (dp->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) != 0 ||
	    sp->slice_type > V4L2_H264_SLICE_TYPE_I || sp->colour_plane_id != 0)
		return (ENOTSUP);
	wmb = sps->pic_width_in_mbs_minus1 + 1;
	hmb = sps->pic_height_in_map_units_minus1 + 1;
	stride = cedrus_stride(ctx->fs.width);
	cedrus_dst_pa = dst->paddr;
	cedrus_dst_stride = stride;
	cedrus_dst_w = ctx->fs.width;
	cedrus_dst_h = ctx->fs.height;
	cedrus_dst_ha = cedrus_cap_height(ctx->fs.height);
	if (wmb * 16 > roundup2(ctx->fs.width, 16) ||
	    hmb * 16 > cedrus_cap_height(ctx->fs.height))
		return (EINVAL);
	if (dst->size < (size_t)cedrus_stride(ctx->fs.width) * cedrus_cap_height(ctx->fs.height) * 3 / 2 ||
	    dst->paddr < DRAM_BASE || dst->paddr + dst->size > DRAM_BASE + ((u_long)cedrus_dma_mb << 20) ||
	    src->paddr < DRAM_BASE || src->paddr + src->size > DRAM_BASE + ((u_long)cedrus_dma_mb << 20) ||
	    src->bytesused < 4 || src->bytesused > src->size ||
	    (src->paddr & 15) != 0 || sp->header_bit_size > (uint32_t)src->bytesused * 8) {
		device_printf(d->sc->dev, "reject: dst %jx+%zx src %jx+%zx used %zu hdr %u\n",
		    (uintmax_t)dst->paddr, dst->size, (uintmax_t)src->paddr, src->size,
		    (size_t)src->bytesused, sp->header_bit_size);
		return (EINVAL);
	}
	/* a free table index for the current picture */
	for (slot = 0; slot < V4L2_H264_NUM_DPB_ENTRIES; slot++)
		if ((dp->dpb[slot].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) == 0)
			break;
	if (slot == V4L2_H264_NUM_DPB_ENTRIES)
		return (EINVAL);
	h->cur_slot = slot;

	/* buffers: two scratch buffers per context, a motion vector buffer per picture */
	geom = (wmb << 16) | hmb;
	/*
	 * F:M019-d: the scratch buffers are far smaller than the original formula
	 * assumed — 64 KiB sufficed at 640x368 with P and B pictures and 128 KiB at
	 * 1280x720, while `wmb * hmb * 1024` asked for 920 KiB and 3.5 MiB.  The
	 * factor is 256 instead: 115 KiB at 640x368 and 450 KiB at 1280x720, still
	 * above every measurement and small enough that two contiguous runs are easy
	 * to place (the 3.5 MiB version needed a 1 GiB window, F:M018-b).
	 */
	scratch = MAX((size_t)128 << 10, (size_t)wmb * hmb * 256);
	if (cedrus_scratch_kb != 0)
		scratch = (size_t)cedrus_scratch_kb << 10;	/* experiments: bisect the real need */
	if (h->geom != geom || h->scratch[0].size < scratch || h->scratch[1].size < scratch) {
		cedrus_mem_free(&h->scratch[0]);
		cedrus_mem_free(&h->scratch[1]);
		if ((error = cedrus_mem_alloc(&h->scratch[0], scratch)) != 0 ||
		    (error = cedrus_mem_alloc(&h->scratch[1], scratch)) != 0)
			return (error);
		h->geom = geom;
	}
	if ((error = cedrus_h264_mv(h, dst->index, wmb, hmb)) != 0)
		return (error);
	cedrus_nref = 0;
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		struct vbuf *ref;

		if ((dp->dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) == 0)
			continue;
		ref = cedrus_ref(ctx, dp->dpb[i].reference_ts, dst);
		if ((error = cedrus_h264_mv(h, ref->index, wmb, hmb)) != 0)
			return (error);
		if (cedrus_nref < 2 && cedrus_h264_refsum != 0) {
			/* experiments: what the engine is about to read */
			cedrus_refsum[2 * cedrus_nref] = cedrus_fnv(
			    (const uint8_t *)PHYS_TO_DMAP(ref->paddr),
			    (size_t)stride * cedrus_cap_height(ctx->fs.height) * 3 / 2);
			cedrus_refsum[2 * cedrus_nref + 1] = cedrus_fnv(
			    (const uint8_t *)PHYS_TO_DMAP(h->mv[ref->index].pa),
			    h->mv[ref->index].size);
			cedrus_nref++;
		}
	}
	return (0);
}

int
cedrus_h264_start(struct cedrus_dec *d, struct cedrus_ctx *ctx, struct vbuf *src,
    struct vbuf *dst)
{
	struct cedrus_h264 *h = ctx->h264;
	const struct v4l2_ctrl_h264_sps *sps = &h->sps;
	const struct v4l2_ctrl_h264_pps *pps = &h->pps;
	const struct v4l2_ctrl_h264_slice_params *sp = &h->sp[0];
	const struct v4l2_ctrl_h264_decode_params *dp = &h->dp;
	uint32_t table[CEDRUS_H264_ENTRIES * 8], words[48];
	uint32_t wmb = sps->pic_width_in_mbs_minus1 + 1;
	uint32_t hmb = sps->pic_height_in_map_units_minus1 + 1;
	uint32_t stride = cedrus_stride(ctx->fs.width);
	bus_addr_t coff = (bus_addr_t)stride * cedrus_cap_height(ctx->fs.height);
	/*
	 * F:M017-a: the two MV halves are wmb x hmb x 16 bytes and that is exactly
	 * how far apart Linux programs them — 0x630 (1584) at 176x144 in five
	 * streams and 0xe100 (57600) at 1280x720 — with the SPS width, not the
	 * picture stride.  M014-k's "3456 corrupts, 3584 is exact" is the size of
	 * the *per-picture* MV buffer (the lab allocated 4096 per picture), not the
	 * spacing: the engine writes past a bare half, so a picture needs about
	 * 2 x 1792 at 176x144, which cedrus_h264_mv's allocation already covers
	 * (F:M017-b).
	 */
	size_t mv_stride = (size_t)wmb * hmb * 16;
	uint32_t l0 = 0, l1 = 0, v, i, n, remaining, k;
	u_int j;
	bool scaling = (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) != 0;
	bool weighted = V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(pps, sp);

	if (sp->slice_type != V4L2_H264_SLICE_TYPE_I)
		l0 = sp->num_ref_idx_l0_active_minus1;
	if (sp->slice_type == V4L2_H264_SLICE_TYPE_B)
		l1 = sp->num_ref_idx_l1_active_minus1;

	if (cedrus_ntr + 400 > CEDRUS_TRACE_MAX)
		cedrus_ntr = 0;
	cedrus_sumva = src->vaddr;
	cedrus_sumlen = src->bytesused;
	cedrus_sum[0] = cedrus_fnv(src->vaddr, src->bytesused);
	cedrus_last[0] = cedrus_rel(src->paddr);
	cedrus_last[1] = cedrus_rel(dst->paddr);
	cedrus_last[2] = cedrus_rel(h->mv[dst->index].pa);
	cedrus_last[3] = cedrus_rel(h->scratch[0].pa);
	cedrus_last[4] = cedrus_rel(h->scratch[1].pa);
	cedrus_last[5] = h->sp[0].first_mb_in_slice;
	cedrus_nsl++;
	if (((cedrus_last[0] ^ cedrus_last[1]) | (cedrus_last[0] ^ cedrus_last[2]) |
	    (cedrus_last[0] ^ cedrus_last[3]) | (cedrus_last[0] ^ cedrus_last[4])) >> 26 != 0)
		cedrus_nmix++;
	if (cedrus_h264_delay != 0)
		DELAY(cedrus_h264_delay);
	if (cedrus_h264_reset != 0) {
		hwreset_assert(d->sc->rst);
		DELAY(10);
		hwreset_deassert(d->sc->rst);
		DELAY(10);
	}
	/* F:M014-e: the table: every reference picture, and the current one at h->cur_slot */
	memset(table, 0, sizeof(table));
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &dp->dpb[i];
		struct vbuf *ref;

		if ((e->flags & V4L2_H264_DPB_ENTRY_FLAG_VALID) == 0)
			continue;
		ref = cedrus_ref(ctx, e->reference_ts, dst);
		table[8 * i + 0] = (uint32_t)e->top_field_order_cnt;
		table[8 * i + 1] = (uint32_t)e->bottom_field_order_cnt;
		table[8 * i + 3] = cedrus_rel(ref->paddr);
		table[8 * i + 4] = cedrus_rel(ref->paddr + coff);
		table[8 * i + 5] = cedrus_rel(h->mv[ref->index].pa);
		table[8 * i + 6] = cedrus_rel(h->mv[ref->index].pa + mv_stride);
	}
	i = h->cur_slot;
	table[8 * i + 0] = (uint32_t)dp->top_field_order_cnt;
	table[8 * i + 1] = (uint32_t)dp->bottom_field_order_cnt;
	table[8 * i + 3] = cedrus_rel(dst->paddr);
	table[8 * i + 4] = cedrus_rel(dst->paddr + coff);
	table[8 * i + 5] = cedrus_rel(h->mv[dst->index].pa);
	table[8 * i + 6] = cedrus_rel(h->mv[dst->index].pa + mv_stride);

	/* F:M014-b, step 2: output format, strides, engine */
	wr(d, VE_OUT_FORMAT, VE_OUT_NV12);
	wr(d, VE_OUT_CHROMA_OFF, (uint32_t)(coff / 4));	/* luma plane size / 4 */
	wr(d, VE_OUT_STRIDE, ((stride / 2) << 16) | stride);
	wr(d, VE_CTRL, VE_CTRL_H264);
	wr(d, H264_ENGINE, 0);
	wr(d, H264_BUF_A, cedrus_rel(h->scratch[0].pa));
	wr(d, H264_BUF_B, cedrus_rel(h->scratch[1].pa));
	/* F:M014-h: scaling matrices, when they apply */
	if (scaling) {
		pack(words, &h->sm.scaling_list_8x8[0][0], 64);
		sram(d, SRAM_H264_SCALE8, words, 16);
		pack(words, &h->sm.scaling_list_8x8[1][0], 64);
		sram(d, SRAM_H264_SCALE8 + 0x40, words, 16);
		pack(words, &h->sm.scaling_list_4x4[0][0], 96);
		sram(d, SRAM_H264_SCALE4, words, 24);
	}
	/* the table, the current index and the data */
	sram(d, SRAM_H264_TABLE, table, nitems(table));
	wr(d, H264_CUR, h->cur_slot);
	wr(d, H264_VLD_LEN, src->bytesused * 8);
	wr(d, H264_VLD_OFFSET, 0);
	wr(d, H264_VBV_END, cedrus_rel(src->paddr + src->bytesused));
	wr(d, H264_VLD_ADDR, cedrus_vld_addr(src->paddr, 7));
	wr(d, H264_X050, 0);
	wr(d, H264_CMD, H264_CMD_RESET);
	/* F:M014-d: skip the start code, the NAL header and the slice header */
	for (remaining = sp->header_bit_size; remaining > 0; remaining -= n) {
		n = remaining < 32 ? remaining : 32;
		wr(d, H264_CMD, H264_CMD_SKIP(n));
		for (k = 0; k < 2000 && (rd(d, H264_STATUS) & H264_STATUS_SKIPPED) == 0; k++)
			DELAY(1);
		if (k == 2000)
			return (EIO);
	}
	/* F:M014-g, F:M015-d/e: explicit weights; list 1 is the second 96 words */
	if (weighted) {
		const struct v4l2_ctrl_h264_pred_weights *pw = &h->pw;
		uint32_t wt[192];

		memset(wt, 0, sizeof(wt));
		for (j = 0; j < (sp->slice_type == V4L2_H264_SLICE_TYPE_B ? 2u : 1u); j++) {
			const struct v4l2_h264_weight_factors *f = &pw->weight_factors[j];
			uint32_t cnt = (j == 0 ? l0 : l1) + 1;

			for (i = 0; i < cnt && i < 32; i++) {
				wt[96 * j + i] = (uint16_t)f->luma_weight[i] |
				    ((uint32_t)(f->luma_offset[i] & 0x1ff) << 16);
				wt[96 * j + 32 + 2 * i] = (uint16_t)f->chroma_weight[i][0] |
				    ((uint32_t)(f->chroma_offset[i][0] & 0x1ff) << 16);
				wt[96 * j + 33 + 2 * i] = (uint16_t)f->chroma_weight[i][1] |
				    ((uint32_t)(f->chroma_offset[i][1] & 0x1ff) << 16);
			}
		}
		wr(d, H264_WDENOM, (pw->chroma_log2_weight_denom << 4) | pw->luma_log2_weight_denom);
		sram(d, SRAM_H264_WEIGHTS, wt, nitems(wt));
	}
	/* F:M014-f: the reference lists, as table indices times two */
	if (sp->slice_type != V4L2_H264_SLICE_TYPE_I) {
		uint8_t lb[32];

		for (i = 0; i <= l0 && i < 32; i++)
			lb[i] = sp->ref_pic_list0[i].index * 2;
		pack(words, lb, l0 + 1);
		sram(d, SRAM_H264_LIST0, words, (l0 + 1 + 3) / 4);
		if (sp->slice_type == V4L2_H264_SLICE_TYPE_B) {
			for (i = 0; i <= l1 && i < 32; i++)
				lb[i] = sp->ref_pic_list1[i].index * 2;
			pack(words, lb, l1 + 1);
			sram(d, SRAM_H264_LIST1, words, (l1 + 1 + 3) / 4);
		}
	}
	/* F:M014-c: the slice registers */
	v = ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) != 0 ? 1U << 15 : 0) |
	    (l0 << 10) | (l1 << 5) |
	    ((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) != 0 ? 1U << 4 : 0) |
	    ((uint32_t)pps->weighted_bipred_idc << 2) |
	    ((pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) != 0 ? 1U << 1 : 0) |
	    ((pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) != 0 ? 1U : 0);
	wr(d, H264_FLAGS, v);
	wr(d, H264_PICSIZE, 0x000d0000 | ((wmb - 1) << 8) | (hmb - 1));
	wr(d, H264_SLICE, ((sp->first_mb_in_slice % wmb) << 24) |
	    ((sp->first_mb_in_slice / wmb) << 16) | (dp->nal_ref_idc != 0 ? 1U << 12 : 0) |
	    ((uint32_t)sp->slice_type << 8) |
	    ((uint32_t)(sp->cabac_init_idc & 3)) |	/* F:M015-a: bits [1:0] */
	    ((sp->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED) != 0 ? 1U << 2 : 0));
	wr(d, H264_DBF, 0x1000 | (l0 << 24) | (l1 << 16) |
	    ((uint32_t)sp->disable_deblocking_filter_idc << 8) |
	    (((uint32_t)sp->slice_alpha_c0_offset_div2 & 15) << 4) |
	    ((uint32_t)sp->slice_beta_offset_div2 & 15));
	wr(d, H264_QP, (scaling ? 0U : 1U << 24) |
	    (((uint32_t)pps->second_chroma_qp_index_offset & 63) << 16) |
	    (((uint32_t)pps->chroma_qp_index_offset & 63) << 8) |
	    (uint32_t)(26 + pps->pic_init_qp_minus26 + sp->slice_qp_delta));
	/* F:M014-b, step 9: acknowledge, enable the interrupts, start */
	if (cedrus_h264_ack == 0)
		wr(d, H264_STATUS, rd(d, H264_STATUS));
	else if (cedrus_h264_ack == 1)
		wr(d, H264_STATUS, rd(d, H264_STATUS) | H264_STATUS_FLAGS);
	else
		wr(d, H264_STATUS, H264_STATUS_FLAGS);
	wr(d, H264_IRQEN, cedrus_h264_poll != 0 ? 0 : 7);
	__asm __volatile("dsb st" ::: "memory");
	cedrus_sum[1] = cedrus_fnv(cedrus_sumva, cedrus_sumlen);
	if ((cedrus_h264_zero & 1) != 0 && src->size > src->bytesused)
		memset((char *)src->vaddr + src->bytesused, 0, src->size - src->bytesused);
	if ((cedrus_h264_zero & 2) != 0 && sp->first_mb_in_slice == 0)
		memset(dst->vaddr, 0, dst->size);
	if ((cedrus_h264_zero & 4) != 0) {
		memset(h->scratch[0].kva, 0, h->scratch[0].size);
		memset(h->scratch[1].kva, 0, h->scratch[1].size);
		memset(h->mv[dst->index].kva, 0, h->mv[dst->index].size);
	}
	if (cedrus_h264_flush != 0) {
		cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(dst->paddr), dst->size);
		cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(src->paddr), src->size);
		cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(h->mv[dst->index].pa), h->mv[dst->index].size);
		cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(h->scratch[0].pa), h->scratch[0].size);
		cpu_dcache_wbinv_range((void *)PHYS_TO_DMAP(h->scratch[1].pa), h->scratch[1].size);
		__asm __volatile("dsb sy" ::: "memory");
	}
	wr(d, H264_CMD, H264_CMD_START);
	if (cedrus_h264_poll != 0) {
		uint32_t st = 0;

		for (k = 0; k < 200000; k++) {
			st = rd(d, H264_STATUS);
			if ((st & (H264_STATUS_FINISH | H264_STATUS_ERROR)) != 0)
				break;
			DELAY(5);
		}
		wr(d, H264_STATUS, H264_STATUS_FLAGS);
		if (k == 200000 || (st & H264_STATUS_ERROR) != 0) {
			device_printf(d->sc->dev, "H.264 slice (polled) status %08x\n", st);
			return (-2);
		}
		if (cedrus_dstn < nitems(cedrus_dstsum))
			cedrus_dstsum[cedrus_dstn++] = cedrus_fnv_frame(cedrus_dst_pa,
			    cedrus_dst_stride, cedrus_dst_w, cedrus_dst_h, cedrus_dst_ha);
		return (-1);
	}
	return (0);
}

int
cedrus_h264_irq(struct cedrus_dec *d, bool *error)
{
	uint32_t st = rd(d, H264_STATUS);

	if ((st & (H264_STATUS_FINISH | H264_STATUS_ERROR)) == 0)
		return (0);
	if (cedrus_h264_post_delay != 0)
		DELAY(cedrus_h264_post_delay);
	/* F:M014-b, step 10 */
	wr(d, H264_IRQEN, 0);
	wr(d, H264_STATUS, H264_STATUS_FLAGS);
	*error = (st & H264_STATUS_ERROR) != 0;
	if (!*error && cedrus_dstn < nitems(cedrus_dstsum))
		cedrus_dstsum[cedrus_dstn++] = cedrus_fnv_frame(cedrus_dst_pa,
		    cedrus_dst_stride, cedrus_dst_w, cedrus_dst_h, cedrus_dst_ha);
	cedrus_sum[2] = cedrus_fnv(cedrus_sumva, cedrus_sumlen);
	{
		static u_int shown2;

		if ((cedrus_sum[0] != cedrus_sum[1] || cedrus_sum[1] != cedrus_sum[2]) && shown2++ < 20)
			device_printf(d->sc->dev, "SLICE DATA CHANGED %08x %08x %08x (status %08x)\n", cedrus_sum[0], cedrus_sum[1], cedrus_sum[2], st);
	}
	{
		static u_int shown;

		if (*error && shown++ < 20)
			device_printf(d->sc->dev, "H.264 slice ended with status %08x: src %08x luma %08x mv %08x scr %08x/%08x mb %u\n", st, cedrus_last[0], cedrus_last[1], cedrus_last[2], cedrus_last[3], cedrus_last[4], cedrus_last[5]);
	}
	return (1);
}
