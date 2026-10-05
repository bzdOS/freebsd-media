/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * h264ctl.h: what a userland H.264 decoder does before it talks to a stateless V4L2
 * decoder, as a header-only library without libc. It reads an Annex B byte stream
 * and, picture by picture, produces the stateless controls of the V4L2 UAPI: the
 * SPS, the PPS, the scaling matrices, the prediction weights, the slice parameters
 * and the decode parameters with the DPB, including everything the controls ask of
 * userland: picture order counts, reference marking (sliding window and MMCO),
 * reference list construction and modification.
 *
 * Scope: frame pictures only (frame_mbs_only_flag 1), 4:2:0, 8 bit, no slice groups,
 * picture order count types 0 and 2. Anything else is refused with a negative
 * return. Used by the Linux register-trace rig (freestanding) and by the FreeBSD
 * test (tests/cedrus_h264.c); include <linux/videodev2.h> first.
 *
 * Offsets: header_bit_size of a slice counts from the first byte of the data handed
 * to the driver, which starts at the start code: (start code + NAL header bytes) * 8
 * plus the bits of the slice header, counting the emulation prevention bytes that lie
 * inside the header.
 */
#ifndef _H264CTL_H_
#define	_H264CTL_H_

#include <stdint.h>
#include <stddef.h>

#define	H264_MAX_SLICES		32
#define	H264_MAX_SPS		32
#define	H264_MAX_PPS		256
#define	H264_RBSP_MAX		(1 << 20)

struct h264_slice_info {
	struct v4l2_ctrl_h264_slice_params	sp;
	struct v4l2_ctrl_h264_pred_weights	pw;
	int					has_pw;
	const uint8_t				*data;		/* start code included */
	uint32_t				len;
};

/* One picture: all its controls and slices. */
struct h264_pic {
	struct v4l2_ctrl_h264_sps		sps;
	struct v4l2_ctrl_h264_pps		pps;
	struct v4l2_ctrl_h264_scaling_matrix	sm;
	int					has_sm;		/* a non-flat matrix applies */
	struct v4l2_ctrl_h264_decode_params	dp;
	struct h264_slice_info			slice[H264_MAX_SLICES];
	unsigned				nslices;
	uint64_t				ts;		/* names this picture's capture buffer */
	int					poc;		/* min(top, bottom): display order key */
	int					idr;
	int					is_ref;
};

/* ------------------------------------------------------------------- state */

struct h264_sps_x {
	int valid;
	struct v4l2_ctrl_h264_sps v;
	int log2_max_frame_num, log2_max_poc_lsb, frame_mbs_only, delta_pic_order_always_zero;
	int width_mbs, height_mbs, max_num_ref_frames, poc_type, direct_8x8;
	int scaling_present;
	uint8_t sl4[6][16], sl8[6][64];		/* raster order after inverse zigzag */
};

struct h264_pps_x {
	int valid;
	struct v4l2_ctrl_h264_pps v;
	int scaling_present, sps_id;
	uint8_t sl4[6][16], sl8[6][64];
	int num_sl8;
};

struct h264_ref {
	int used;
	int frame_num, frame_num_wrap, long_term, long_idx;
	int poc, top, bottom;
	uint64_t ts;
	int is_ref;			/* still marked "used for reference" */
	int order;			/* insertion order, for debugging */
};

struct h264_ctx {
	struct h264_sps_x	sps[H264_MAX_SPS];
	struct h264_pps_x	pps[H264_MAX_PPS];
	struct h264_ref		dpb[16];
	int			prev_ref_frame_num;
	int			prev_poc_msb, prev_poc_lsb;
	int			frame_num_offset, prev_frame_num_offset, prev_frame_num;
	int			max_long_idx;
	uint8_t			rbsp[H264_RBSP_MAX];
	int			epb[4096];		/* rbsp indices where a 0x03 was removed */
	int			nepb;
	int			norder;
};

/* ----------------------------------------------------------------- bit reader */

struct h264_br {
	const uint8_t	*b;
	uint32_t	nbits, pos;
	int		err;
};

static inline uint32_t
h264_u(struct h264_br *r, int n)
{
	uint32_t v = 0;

	while (n-- > 0) {
		if (r->pos >= r->nbits) {
			r->err = 1;
			return (0);
		}
		v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
		r->pos++;
	}
	return (v);
}

static inline uint32_t
h264_ue(struct h264_br *r)
{
	int z = 0;
	uint32_t v;

	while (h264_u(r, 1) == 0 && !r->err)
		if (++z > 31) {
			r->err = 1;
			return (0);
		}
	if (z == 0)
		return (0);
	v = h264_u(r, z);
	return ((1u << z) - 1 + v);
}

static inline int32_t
h264_se(struct h264_br *r)
{
	uint32_t k = h264_ue(r);

	return ((k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2));
}

static void
h264_zero(void *p, size_t n)
{
	uint8_t *q = p;

	while (n--)
		*q++ = 0;
}

static void
h264_copy(void *d, const void *s, size_t n)
{
	uint8_t *dd = d;
	const uint8_t *ss = s;

	while (n--)
		*dd++ = *ss++;
}

/* NAL payload (after the NAL header byte) to RBSP; records where bytes were dropped. */
static uint32_t
h264_unescape(struct h264_ctx *c, const uint8_t *p, uint32_t n)
{
	uint32_t i, o = 0;
	int zeros = 0;

	c->nepb = 0;
	for (i = 0; i < n && o < H264_RBSP_MAX; i++) {
		if (zeros >= 2 && p[i] == 3) {
			if (c->nepb < 4096)
				c->epb[c->nepb++] = (int)o;
			zeros = 0;
			continue;
		}
		zeros = p[i] == 0 ? zeros + 1 : 0;
		c->rbsp[o++] = p[i];
	}
	return (o);
}

/* A bit position in the RBSP as a position in the escaped bytes. */
static uint32_t
h264_raw_bits(const struct h264_ctx *c, uint32_t rbsp_bits)
{
	uint32_t bytes = rbsp_bits / 8, k, n = 0;

	for (k = 0; k < (uint32_t)c->nepb; k++)
		if ((uint32_t)c->epb[k] <= bytes)
			n++;
	return (rbsp_bits + 8 * n);
}

/* --------------------------------------------------------------- parameter sets */

static const uint8_t h264_zz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t h264_zz8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/*
 * scaling_list(): the list in zigzag order -> raster. Returns 1 if the "default"
 * flag came up (useDefaultScalingMatrixFlag).
 */
static int
h264_scaling_list(struct h264_br *r, uint8_t *out, int size)
{
	int last = 8, next = 8, j, use_default = 0;
	uint8_t zz[64];

	for (j = 0; j < size; j++) {
		if (next != 0) {
			int d = h264_se(r);

			next = (last + d + 256) % 256;
			use_default = (j == 0 && next == 0);
		}
		zz[j] = next == 0 ? last : next;
		last = zz[j];
	}
	for (j = 0; j < size; j++)
		out[size == 16 ? h264_zz4[j] : h264_zz8[j]] = zz[j];
	return (use_default);
}

static const uint8_t h264_def4_intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
static const uint8_t h264_def4_inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
static const uint8_t h264_def8_intra[64] = {
	6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 };
static const uint8_t h264_def8_inter[64] = {
	9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 };

/* Default lists are given in zigzag order in the tables above; convert to raster. */
static void
h264_default_raster(uint8_t *out, const uint8_t *zz, int size)
{
	int j;

	for (j = 0; j < size; j++)
		out[size == 16 ? h264_zz4[j] : h264_zz8[j]] = zz[j];
}

/*
 * The matrices of a parameter set. `fall` supplies the fall-back lists (the SPS's
 * for a PPS, NULL for an SPS where the defaults apply). n8 = number of 8x8 lists.
 */
static int
h264_scaling_matrix(struct h264_br *r, uint8_t sl4[6][16], uint8_t sl8[6][64], int n8,
    const uint8_t (*f4)[16], const uint8_t (*f8)[64], int pps_fallback)
{
	int i, use_def;
	uint8_t flag[12];

	for (i = 0; i < 6 + n8; i++) {
		flag[i] = h264_u(r, 1);
		if (!flag[i])
			continue;
		if (i < 6)
			use_def = h264_scaling_list(r, sl4[i], 16);
		else
			use_def = h264_scaling_list(r, sl8[i - 6], 64);
		if (use_def) {
			if (i < 6)
				h264_default_raster(sl4[i], i < 3 ? h264_def4_intra : h264_def4_inter, 16);
			else
				h264_default_raster(sl8[i - 6], ((i - 6) & 1) == 0 ? h264_def8_intra : h264_def8_inter, 64);
		}
	}
	/* fall-back rule A (SPS) or B (PPS) for lists that were not sent */
	for (i = 0; i < 6; i++) {
		if (flag[i])
			continue;
		if (i == 0 || i == 3) {
			if (pps_fallback)
				h264_copy(sl4[i], f4[i], 16);
			else
				h264_default_raster(sl4[i], i == 0 ? h264_def4_intra : h264_def4_inter, 16);
		} else
			h264_copy(sl4[i], sl4[i - 1], 16);
	}
	for (i = 0; i < n8; i++) {
		if (flag[6 + i])
			continue;
		if (i < 2) {
			if (pps_fallback)
				h264_copy(sl8[i], f8[i], 64);
			else
				h264_default_raster(sl8[i], i == 0 ? h264_def8_intra : h264_def8_inter, 64);
		} else
			h264_copy(sl8[i], sl8[i - 2], 64);
	}
	return (r->err);
}

static void
h264_flat(uint8_t sl4[6][16], uint8_t sl8[6][64])
{
	int i, j;

	for (i = 0; i < 6; i++) {
		for (j = 0; j < 16; j++)
			sl4[i][j] = 16;
		for (j = 0; j < 64; j++)
			sl8[i][j] = 16;
	}
}

static int
h264_parse_sps(struct h264_ctx *c, const uint8_t *nal, uint32_t n)
{
	struct h264_br r;
	struct h264_sps_x *s;
	struct v4l2_ctrl_h264_sps *v;
	uint32_t len, id, i;
	int profile;

	len = h264_unescape(c, nal + 1, n - 1);
	r.b = c->rbsp; r.nbits = len * 8; r.pos = 0; r.err = 0;
	profile = h264_u(&r, 8);
	{
		uint32_t cs = h264_u(&r, 8), level = h264_u(&r, 8);

		id = h264_ue(&r);
		if (id >= H264_MAX_SPS || r.err)
			return (-1);
		s = &c->sps[id];
		h264_zero(s, sizeof(*s));
		v = &s->v;
		v->profile_idc = profile;
		v->constraint_set_flags = cs >> 2;
		v->level_idc = level;
		v->seq_parameter_set_id = id;
	}
	v->chroma_format_idc = 1;
	h264_flat(s->sl4, s->sl8);
	if (V4L2_H264_SPS_HAS_CHROMA_FORMAT(v)) {
		v->chroma_format_idc = h264_ue(&r);
		if (v->chroma_format_idc == 3)
			if (h264_u(&r, 1))
				v->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
		v->bit_depth_luma_minus8 = h264_ue(&r);
		v->bit_depth_chroma_minus8 = h264_ue(&r);
		if (h264_u(&r, 1))
			v->flags |= V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
		if (h264_u(&r, 1)) {
			s->scaling_present = 1;
			h264_scaling_matrix(&r, s->sl4, s->sl8, v->chroma_format_idc != 3 ? 2 : 6,
			    NULL, NULL, 0);
		}
	}
	if (v->chroma_format_idc != 1 || v->bit_depth_luma_minus8 != 0 ||
	    v->bit_depth_chroma_minus8 != 0 || (v->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE))
		return (-2);			/* 4:2:0, 8 bit only */
	v->log2_max_frame_num_minus4 = h264_ue(&r);
	s->log2_max_frame_num = v->log2_max_frame_num_minus4 + 4;
	v->pic_order_cnt_type = h264_ue(&r);
	s->poc_type = v->pic_order_cnt_type;
	if (v->pic_order_cnt_type == 0) {
		v->log2_max_pic_order_cnt_lsb_minus4 = h264_ue(&r);
		s->log2_max_poc_lsb = v->log2_max_pic_order_cnt_lsb_minus4 + 4;
	} else if (v->pic_order_cnt_type == 1) {
		return (-3);			/* poc type 1: not supported */
	}
	v->max_num_ref_frames = h264_ue(&r);
	s->max_num_ref_frames = v->max_num_ref_frames;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
	v->pic_width_in_mbs_minus1 = h264_ue(&r);
	v->pic_height_in_map_units_minus1 = h264_ue(&r);
	s->frame_mbs_only = h264_u(&r, 1);
	if (!s->frame_mbs_only)
		return (-4);			/* interlaced coding: not supported */
	v->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	s->direct_8x8 = (v->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) != 0;
	s->width_mbs = v->pic_width_in_mbs_minus1 + 1;
	s->height_mbs = v->pic_height_in_map_units_minus1 + 1;
	(void)i;
	if (r.err)
		return (-5);
	s->valid = 1;
	return (0);
}

static int
h264_parse_pps(struct h264_ctx *c, const uint8_t *nal, uint32_t n)
{
	struct h264_br r;
	struct h264_pps_x *p;
	struct v4l2_ctrl_h264_pps *v;
	uint32_t len, id, sid;

	len = h264_unescape(c, nal + 1, n - 1);
	r.b = c->rbsp; r.nbits = len * 8; r.pos = 0; r.err = 0;
	id = h264_ue(&r);
	sid = h264_ue(&r);
	if (id >= H264_MAX_PPS || sid >= H264_MAX_SPS || r.err || !c->sps[sid].valid)
		return (-1);
	p = &c->pps[id];
	h264_zero(p, sizeof(*p));
	v = &p->v;
	v->pic_parameter_set_id = id;
	v->seq_parameter_set_id = sid;
	p->sps_id = sid;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	v->num_slice_groups_minus1 = h264_ue(&r);
	if (v->num_slice_groups_minus1 != 0)
		return (-2);			/* slice groups: not supported */
	v->num_ref_idx_l0_default_active_minus1 = h264_ue(&r);
	v->num_ref_idx_l1_default_active_minus1 = h264_ue(&r);
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	v->weighted_bipred_idc = h264_u(&r, 2);
	v->pic_init_qp_minus26 = h264_se(&r);
	v->pic_init_qs_minus26 = h264_se(&r);
	v->chroma_qp_index_offset = h264_se(&r);
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	v->second_chroma_qp_index_offset = v->chroma_qp_index_offset;
	/* the optional tail: more_rbsp_data() */
	{
		uint32_t last = len * 8, k;

		/* strip the rbsp_trailing_bits: the last 1 bit and the zeros after it */
		while (last > 0 && ((c->rbsp[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1) == 0)
			last--;
		if (last > 0)
			last--;
		k = r.pos;
		if (k < last) {
			if (h264_u(&r, 1))
				v->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
			if (h264_u(&r, 1)) {
				struct h264_sps_x *s = &c->sps[sid];
				int n8 = (v->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) ? 2 : 0;

				p->scaling_present = 1;
				v->flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
				p->num_sl8 = n8;
				h264_flat(p->sl4, p->sl8);
				h264_scaling_matrix(&r, p->sl4, p->sl8, n8,
				    s->scaling_present ? (const uint8_t (*)[16])s->sl4 :
				    (const uint8_t (*)[16])p->sl4,
				    s->scaling_present ? (const uint8_t (*)[64])s->sl8 :
				    (const uint8_t (*)[64])p->sl8, s->scaling_present);
			}
			v->second_chroma_qp_index_offset = h264_se(&r);
		}
	}
	if (r.err)
		return (-3);
	p->valid = 1;
	return (0);
}

/* --------------------------------------------------------------------- slices */

struct h264_sh {
	int nal_ref_idc, idr, nal_type;
	uint32_t first_mb, slice_type, pps_id, frame_num, idr_pic_id;
	int poc_lsb, delta_poc_bottom, delta_poc[2];
	int redundant_pic_cnt, direct_spatial, override, nref[2];
	int mod_n[2], mod_idc[2][34], mod_val[2][34];
	int cabac_init_idc, qp_delta, qs_delta, dbf_idc, alpha, beta;
	int no_output_prior, long_term_ref_flag, adaptive;
	int mmco_n, mmco_op[34], mmco_a[34], mmco_b[34];
	uint32_t hdr_bits;		/* RBSP bits up to slice_data() */
	uint32_t poc_bits, marking_bits;
	struct v4l2_ctrl_h264_pred_weights pw;
	int has_pw;
};

static int
h264_pred_weight_table(struct h264_br *r, struct h264_sh *h, int nl, const int nref[2])
{
	int l, i, k;

	h->pw.luma_log2_weight_denom = h264_ue(r);
	h->pw.chroma_log2_weight_denom = h264_ue(r);
	for (l = 0; l < nl; l++)
		for (i = 0; i <= nref[l]; i++) {
			struct v4l2_h264_weight_factors *f = &h->pw.weight_factors[l];

			f->luma_weight[i] = 1 << h->pw.luma_log2_weight_denom;
			f->luma_offset[i] = 0;
			if (h264_u(r, 1)) {
				f->luma_weight[i] = h264_se(r);
				f->luma_offset[i] = h264_se(r);
			}
			for (k = 0; k < 2; k++) {
				f->chroma_weight[i][k] = 1 << h->pw.chroma_log2_weight_denom;
				f->chroma_offset[i][k] = 0;
			}
			if (h264_u(r, 1))
				for (k = 0; k < 2; k++) {
					f->chroma_weight[i][k] = h264_se(r);
					f->chroma_offset[i][k] = h264_se(r);
				}
		}
	return (r->err);
}

static int
h264_parse_slice_header(struct h264_ctx *c, const uint8_t *nal, uint32_t n, struct h264_sh *h,
    struct h264_pps_x **ppsp, struct h264_sps_x **spsp)
{
	struct h264_br r;
	struct h264_pps_x *p;
	struct h264_sps_x *s;
	uint32_t len, st, l, nl, pos0;
	int i;

	h264_zero(h, sizeof(*h));
	h->nal_type = nal[0] & 31;
	h->nal_ref_idc = (nal[0] >> 5) & 3;
	h->idr = h->nal_type == 5;
	len = h264_unescape(c, nal + 1, n - 1);
	r.b = c->rbsp; r.nbits = len * 8; r.pos = 0; r.err = 0;
	h->first_mb = h264_ue(&r);
	st = h264_ue(&r);
	h->slice_type = st % 5;
	h->pps_id = h264_ue(&r);
	if (h->pps_id >= H264_MAX_PPS || !c->pps[h->pps_id].valid)
		return (-1);
	p = &c->pps[h->pps_id];
	s = &c->sps[p->sps_id];
	h->frame_num = h264_u(&r, s->log2_max_frame_num);
	if (h->idr)
		h->idr_pic_id = h264_ue(&r);
	pos0 = r.pos;
	if (s->poc_type == 0) {
		h->poc_lsb = h264_u(&r, s->log2_max_poc_lsb);
		if ((p->v.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT))
			h->delta_poc_bottom = h264_se(&r);
	}
	h->poc_bits = r.pos - pos0;
	if ((p->v.flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT))
		h->redundant_pic_cnt = h264_ue(&r);
	nl = h->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 : (h->slice_type == V4L2_H264_SLICE_TYPE_I ? 0 : 1);
	if (h->slice_type == V4L2_H264_SLICE_TYPE_B)
		h->direct_spatial = h264_u(&r, 1);
	h->nref[0] = p->v.num_ref_idx_l0_default_active_minus1;
	h->nref[1] = p->v.num_ref_idx_l1_default_active_minus1;
	if (nl > 0) {
		h->override = h264_u(&r, 1);
		if (h->override) {
			h->nref[0] = h264_ue(&r);
			if (nl == 2)
				h->nref[1] = h264_ue(&r);
		}
	}
	for (l = 0; l < nl; l++) {
		if (h264_u(&r, 1)) {
			for (i = 0; i < 33; i++) {
				int idc = h264_ue(&r);

				if (idc == 3 || r.err)
					break;
				h->mod_idc[l][h->mod_n[l]] = idc;
				h->mod_val[l][h->mod_n[l]] = h264_ue(&r);
				h->mod_n[l]++;
			}
		}
	}
	if (((p->v.flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) &&
	    (h->slice_type == V4L2_H264_SLICE_TYPE_P || h->slice_type == V4L2_H264_SLICE_TYPE_SP)) ||
	    (p->v.weighted_bipred_idc == 1 && h->slice_type == V4L2_H264_SLICE_TYPE_B)) {
		h264_pred_weight_table(&r, h, nl, h->nref);
		h->has_pw = 1;
	}
	if (h->nal_ref_idc != 0) {
		uint32_t m0 = r.pos;

		if (h->idr) {
			h->no_output_prior = h264_u(&r, 1);
			h->long_term_ref_flag = h264_u(&r, 1);
		} else {
			h->adaptive = h264_u(&r, 1);
			if (h->adaptive)
				for (i = 0; i < 33; i++) {
					int op = h264_ue(&r);

					if (op == 0 || r.err)
						break;
					h->mmco_op[h->mmco_n] = op;
					h->mmco_a[h->mmco_n] = h->mmco_b[h->mmco_n] = 0;
					if (op == 1 || op == 3)
						h->mmco_a[h->mmco_n] = h264_ue(&r);	/* difference_of_pic_nums_minus1 */
					if (op == 2)
						h->mmco_a[h->mmco_n] = h264_ue(&r);	/* long_term_pic_num */
					if (op == 3 || op == 6)
						h->mmco_b[h->mmco_n] = h264_ue(&r);	/* long_term_frame_idx */
					if (op == 4)
						h->mmco_a[h->mmco_n] = h264_ue(&r);	/* max_long_term_frame_idx_plus1 */
					h->mmco_n++;
				}
		}
		h->marking_bits = r.pos - m0;
	}
	if ((p->v.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) &&
	    h->slice_type != V4L2_H264_SLICE_TYPE_I && h->slice_type != V4L2_H264_SLICE_TYPE_SI)
		h->cabac_init_idc = h264_ue(&r);
	h->qp_delta = h264_se(&r);
	if (h->slice_type == V4L2_H264_SLICE_TYPE_SP || h->slice_type == V4L2_H264_SLICE_TYPE_SI)
		return (-2);
	if ((p->v.flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT)) {
		h->dbf_idc = h264_ue(&r);
		if (h->dbf_idc != 1) {
			h->alpha = h264_se(&r);
			h->beta = h264_se(&r);
		}
	}
	h->hdr_bits = r.pos;
	if (r.err)
		return (-3);
	*ppsp = p;
	*spsp = s;
	return (0);
}

/* ----------------------------------------------------------- POC and the DPB */

static void
h264_compute_poc(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, int *top, int *bot)
{
	if (s->poc_type == 0) {
		int max_lsb = 1 << s->log2_max_poc_lsb, msb, pmsb, plsb;

		if (h->idr) {
			pmsb = plsb = 0;
		} else {
			pmsb = c->prev_poc_msb;
			plsb = c->prev_poc_lsb;
		}
		if (h->poc_lsb < plsb && plsb - h->poc_lsb >= max_lsb / 2)
			msb = pmsb + max_lsb;
		else if (h->poc_lsb > plsb && h->poc_lsb - plsb > max_lsb / 2)
			msb = pmsb - max_lsb;
		else
			msb = pmsb;
		*top = msb + h->poc_lsb;
		*bot = *top + h->delta_poc_bottom;
		if (h->nal_ref_idc != 0) {
			c->prev_poc_msb = msb;
			c->prev_poc_lsb = h->poc_lsb;
		}
	} else {			/* type 2 */
		int max_fn = 1 << s->log2_max_frame_num, fno, t;

		if (h->idr)
			fno = 0;
		else if (c->prev_frame_num > (int)h->frame_num)
			fno = c->prev_frame_num_offset + max_fn;
		else
			fno = c->prev_frame_num_offset;
		t = h->idr ? 0 : (h->nal_ref_idc == 0 ? 2 * (fno + h->frame_num) - 1 : 2 * (fno + h->frame_num));
		*top = *bot = t;
		c->prev_frame_num_offset = fno;
		c->prev_frame_num = h->frame_num;
	}
}

static void
h264_wrap_frame_nums(struct h264_ctx *c, const struct h264_sps_x *s, int cur_frame_num)
{
	int i, max_fn = 1 << s->log2_max_frame_num;

	for (i = 0; i < 16; i++)
		if (c->dpb[i].used && c->dpb[i].is_ref && !c->dpb[i].long_term)
			c->dpb[i].frame_num_wrap = c->dpb[i].frame_num > cur_frame_num ?
			    c->dpb[i].frame_num - max_fn : c->dpb[i].frame_num;
}

/* Sort helper: insertion sort of indices by key. */
static void
h264_sort(int *idx, int n, const int *key, int desc)
{
	int i, j, t;

	for (i = 1; i < n; i++) {
		t = idx[i];
		for (j = i - 1; j >= 0 && (desc ? key[idx[j]] < key[t] : key[idx[j]] > key[t]); j--)
			idx[j + 1] = idx[j];
		idx[j + 1] = t;
	}
}

/* 8.2.4: initial lists then modification. Entries are dpb[] slot numbers or -1. */
static void
h264_ref_lists(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, int cur_poc,
    int list[2][34], int nlist[2])
{
	int i, l, n, key[16], sidx[16], lidx[16], ns = 0, nlg = 0;
	int max_fn = 1 << s->log2_max_frame_num;

	for (l = 0; l < 2; l++) {
		nlist[l] = 0;
		for (i = 0; i < 34; i++)
			list[l][i] = -1;
	}
	if (h->slice_type == V4L2_H264_SLICE_TYPE_I)
		return;
	for (i = 0; i < 16; i++) {
		if (!c->dpb[i].used || !c->dpb[i].is_ref)
			continue;
		if (c->dpb[i].long_term) {
			key[i] = c->dpb[i].long_idx;
			lidx[nlg++] = i;
		} else {
			sidx[ns++] = i;
		}
	}
	h264_sort(lidx, nlg, key, 0);
	if (h->slice_type == V4L2_H264_SLICE_TYPE_P) {
		for (i = 0; i < ns; i++)
			key[sidx[i]] = c->dpb[sidx[i]].frame_num_wrap;
		h264_sort(sidx, ns, key, 1);
		n = 0;
		for (i = 0; i < ns; i++)
			list[0][n++] = sidx[i];
		for (i = 0; i < nlg; i++)
			list[0][n++] = lidx[i];
		nlist[0] = n;
	} else {
		int before[16], after[16], nb = 0, na = 0, poc[16];

		for (i = 0; i < ns; i++) {
			poc[sidx[i]] = c->dpb[sidx[i]].poc;
			if (c->dpb[sidx[i]].poc < cur_poc)
				before[nb++] = sidx[i];
			else
				after[na++] = sidx[i];
		}
		h264_sort(before, nb, poc, 1);
		h264_sort(after, na, poc, 0);
		n = 0;
		for (i = 0; i < nb; i++)
			list[0][n++] = before[i];
		for (i = 0; i < na; i++)
			list[0][n++] = after[i];
		for (i = 0; i < nlg; i++)
			list[0][n++] = lidx[i];
		nlist[0] = n;
		n = 0;
		for (i = 0; i < na; i++)
			list[1][n++] = after[i];
		for (i = 0; i < nb; i++)
			list[1][n++] = before[i];
		for (i = 0; i < nlg; i++)
			list[1][n++] = lidx[i];
		nlist[1] = n;
		if (nlist[1] > 1) {
			int same = 1;

			for (i = 0; i < nlist[1]; i++)
				if (list[0][i] != list[1][i])
					same = 0;
			if (same) {
				int t = list[1][0];

				list[1][0] = list[1][1];
				list[1][1] = t;
			}
		}
	}
	/* truncate to the active size, pad with "no reference" */
	for (l = 0; l < (h->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 : 1); l++) {
		int act = h->nref[l] + 1, pred, k, nopic;

		if (nlist[l] > act)
			nlist[l] = act;
		/* modification, 8.2.4.3 */
		pred = (int)h->frame_num;
		(void)max_fn;
		nopic = 0;
		for (k = 0; k < h->mod_n[l]; k++) {
			int idc = h->mod_idc[l][k], val = h->mod_val[l][k], target = -1, no_wrap, picnum, j, c2;

			if (idc == 0 || idc == 1) {
				int d = val + 1;

				if (idc == 0) {
					no_wrap = pred - d;
					if (no_wrap < 0)
						no_wrap += max_fn;
				} else {
					no_wrap = pred + d;
					if (no_wrap >= max_fn)
						no_wrap -= max_fn;
				}
				pred = no_wrap;
				picnum = no_wrap > (int)h->frame_num ? no_wrap - max_fn : no_wrap;
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && !c->dpb[j].long_term &&
					    c->dpb[j].frame_num_wrap == picnum)
						target = j;
			} else {		/* idc 2: long term */
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && c->dpb[j].long_term &&
					    c->dpb[j].long_idx == val)
						target = j;
			}
			if (target < 0)
				continue;
			/* insert at position k, shift the rest, drop the duplicate */
			for (c2 = act; c2 > k; c2--)
				list[l][c2] = list[l][c2 - 1];
			list[l][k] = target;
			{
				int nidx = k + 1;

				for (c2 = k + 1; c2 <= act; c2++)
					if (list[l][c2] != target)
						list[l][nidx++] = list[l][c2];
			}
		}
		nlist[l] = act;
		(void)nopic;
		for (i = 0; i < act; i++)
			if (i >= 34)
				break;
	}
}

/* Marking after a reference picture was decoded: 8.2.5. `slot` is where it goes. */
static void
h264_mark(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, struct h264_ref *cur)
{
	int i, max_fn = 1 << s->log2_max_frame_num, mmco5 = 0;

	if (h->idr) {
		for (i = 0; i < 16; i++)
			c->dpb[i].is_ref = 0;
		if (h->long_term_ref_flag) {
			cur->long_term = 1;
			cur->long_idx = 0;
			c->max_long_idx = 0;
		} else {
			c->max_long_idx = -1;
		}
	} else if (!h->adaptive) {
		int nref = 0, oldest = -1, best = 0x7fffffff;

		h264_wrap_frame_nums(c, s, h->frame_num);
		for (i = 0; i < 16; i++)
			if (c->dpb[i].used && c->dpb[i].is_ref) {
				nref++;
				if (!c->dpb[i].long_term && c->dpb[i].frame_num_wrap < best) {
					best = c->dpb[i].frame_num_wrap;
					oldest = i;
				}
			}
		if (nref >= (s->max_num_ref_frames ? s->max_num_ref_frames : 1) && oldest >= 0)
			c->dpb[oldest].is_ref = 0;
	} else {
		int k;

		h264_wrap_frame_nums(c, s, h->frame_num);
		for (k = 0; k < h->mmco_n; k++) {
			int op = h->mmco_op[k], a = h->mmco_a[k], b = h->mmco_b[k], j;

			if (op == 1) {
				int pn = (int)h->frame_num - (a + 1);

				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && !c->dpb[j].long_term &&
					    c->dpb[j].frame_num_wrap == pn)
						c->dpb[j].is_ref = 0;
			} else if (op == 2) {
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && c->dpb[j].long_term &&
					    c->dpb[j].long_idx == a)
						c->dpb[j].is_ref = 0;
			} else if (op == 3) {
				int pn = (int)h->frame_num - (a + 1);

				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && c->dpb[j].long_term &&
					    c->dpb[j].long_idx == b)
						c->dpb[j].is_ref = 0;
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && !c->dpb[j].long_term &&
					    c->dpb[j].frame_num_wrap == pn) {
						c->dpb[j].long_term = 1;
						c->dpb[j].long_idx = b;
					}
			} else if (op == 4) {
				c->max_long_idx = a - 1;
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && c->dpb[j].long_term &&
					    c->dpb[j].long_idx > c->max_long_idx)
						c->dpb[j].is_ref = 0;
			} else if (op == 5) {
				for (j = 0; j < 16; j++)
					c->dpb[j].is_ref = 0;
				c->max_long_idx = -1;
				mmco5 = 1;
			} else if (op == 6) {
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && c->dpb[j].is_ref && c->dpb[j].long_term &&
					    c->dpb[j].long_idx == b)
						c->dpb[j].is_ref = 0;
				cur->long_term = 1;
				cur->long_idx = b;
			}
		}
	}
	(void)max_fn;
	(void)mmco5;
	cur->is_ref = 1;
}

static int
h264_free_slot(struct h264_ctx *c)
{
	int i, victim = -1, best = 0x7fffffff;

	for (i = 0; i < 16; i++)
		if (!c->dpb[i].used)
			return (i);
	/* DPB full: reuse the oldest entry that is no longer a reference */
	for (i = 0; i < 16; i++)
		if (!c->dpb[i].is_ref && c->dpb[i].order < best) {
			best = c->dpb[i].order;
			victim = i;
		}
	return (victim >= 0 ? victim : 0);
}

/* ------------------------------------------------------------ the controls */

/*
 * Parse the next picture of an Annex B stream starting at *pos. Returns 1 with
 * `out` filled (and *pos after the picture), 0 at the end of the stream, negative
 * on a stream this library does not handle. `ts` names the capture buffer the
 * picture will be decoded into.
 */
static int
h264_next_picture(struct h264_ctx *c, const uint8_t *buf, size_t len, size_t *pos, struct h264_pic *out,
    uint64_t ts)
{
	size_t p = *pos;
	int have = 0, err, i;
	struct h264_sh first;
	struct h264_pps_x *pps = NULL;
	struct h264_sps_x *sps = NULL;
	int top = 0, bot = 0;

	h264_zero(out, sizeof(*out));
	h264_zero(&first, sizeof(first));
	for (;;) {
		size_t sc, nal, next, end;
		int sclen;
		uint32_t nlen;

		/* find the next start code at or after p */
		sc = p;
		while (sc + 3 <= len && !(buf[sc] == 0 && buf[sc + 1] == 0 && buf[sc + 2] == 1))
			sc++;
		if (sc + 3 > len)
			break;
		sclen = (sc > 0 && buf[sc - 1] == 0) ? 4 : 3;
		nal = sc + 3;
		/* the NAL ends at the next start code (3 zero bytes also end it) */
		next = nal + 1;
		while (next + 3 <= len && !(buf[next] == 0 && buf[next + 1] == 0 &&
		    (buf[next + 2] == 1 || buf[next + 2] == 0)))
			next++;
		end = next + 3 <= len ? next : len;
		nlen = (uint32_t)(end - nal);
		{
			int type = buf[nal] & 31;

			if (type == 7) {
				if ((err = h264_parse_sps(c, buf + nal, nlen)) != 0)
					return (err);
			} else if (type == 8) {
				if ((err = h264_parse_pps(c, buf + nal, nlen)) != 0)
					return (err);
			} else if (type == 1 || type == 5) {
				struct h264_sh h;
				struct h264_pps_x *pp;
				struct h264_sps_x *ss;

				if ((err = h264_parse_slice_header(c, buf + nal, nlen, &h, &pp, &ss)) != 0)
					return (err);
				if (have && h.first_mb == 0) {
					*pos = (size_t)(sc - (sclen == 4 ? 1 : 0));
					goto done;
				}
				if (!have) {
					first = h;
					pps = pp;
					sps = ss;
					have = 1;
					out->sps = ss->v;
					out->pps = pp->v;
					h264_compute_poc(c, ss, &h, &top, &bot);
				}
				if (out->nslices >= H264_MAX_SLICES)
					return (-10);
				{
					struct h264_slice_info *si = &out->slice[out->nslices];
					struct v4l2_ctrl_h264_slice_params *sp = &si->sp;
					uint32_t raw_hdr;

					/* position in the escaped bytes, plus start code and NAL header */
					c->nepb = 0;
					(void)h264_unescape(c, buf + nal + 1, nlen - 1);
					raw_hdr = h264_raw_bits(c, h.hdr_bits);
					sp->header_bit_size = raw_hdr + 8 * (uint32_t)(sclen + 1);
					sp->first_mb_in_slice = h.first_mb;
					sp->slice_type = h.slice_type;
					sp->redundant_pic_cnt = h.redundant_pic_cnt;
					sp->cabac_init_idc = h.cabac_init_idc;
					sp->slice_qp_delta = h.qp_delta;
					sp->slice_qs_delta = h.qs_delta;
					sp->disable_deblocking_filter_idc = h.dbf_idc;
					sp->slice_alpha_c0_offset_div2 = h.alpha;
					sp->slice_beta_offset_div2 = h.beta;
					sp->num_ref_idx_l0_active_minus1 = h.nref[0];
					sp->num_ref_idx_l1_active_minus1 = h.nref[1];
					if (h.direct_spatial)
						sp->flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
					si->data = buf + sc - (sclen == 4 ? 1 : 0);
					si->len = (uint32_t)(end - (sc - (sclen == 4 ? 1 : 0)));
					if (h.has_pw) {
						si->pw = h.pw;
						si->has_pw = 1;
					}
					out->nslices++;
					/* lists are made after the DPB is fixed for this picture; keep h */
					if (out->nslices == 1)
						first = h;
					/* remember each slice's header for the list construction below */
					{
						int list[2][34], nlist[2], l, k;

						h264_wrap_frame_nums(c, ss, (int)h.frame_num);
						h264_ref_lists(c, ss, &h, top < bot ? top : bot, list, nlist);
						for (l = 0; l < 2; l++)
							for (k = 0; k < 32; k++) {
								struct v4l2_h264_reference *ref = l == 0 ?
								    &sp->ref_pic_list0[k] : &sp->ref_pic_list1[k];

								if (k < nlist[l] && list[l][k] >= 0) {
									ref->fields = V4L2_H264_FRAME_REF;
									ref->index = list[l][k];
								}
							}
					}
				}
			}
		}
		p = end;
	}
	if (!have)
		return (0);
	*pos = len;
done:
	/* the picture-level controls */
	{
		struct v4l2_ctrl_h264_decode_params *dp = &out->dp;
		int slot;

		dp->nal_ref_idc = first.nal_ref_idc;
		dp->frame_num = first.frame_num;
		dp->top_field_order_cnt = top;
		dp->bottom_field_order_cnt = bot;
		dp->idr_pic_id = first.idr_pic_id;
		dp->pic_order_cnt_lsb = first.poc_lsb;
		dp->delta_pic_order_cnt_bottom = first.delta_poc_bottom;
		dp->delta_pic_order_cnt0 = first.delta_poc[0];
		dp->delta_pic_order_cnt1 = first.delta_poc[1];
		dp->dec_ref_pic_marking_bit_size = first.marking_bits;
		dp->pic_order_cnt_bit_size = first.poc_bits;
		if (first.idr)
			dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
		for (i = 0; i < (int)out->nslices; i++) {
			if (out->slice[i].sp.slice_type == V4L2_H264_SLICE_TYPE_P)
				dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
			if (out->slice[i].sp.slice_type == V4L2_H264_SLICE_TYPE_B)
				dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
		}
		/* the DPB as it stands before this picture */
		h264_wrap_frame_nums(c, sps, (int)first.frame_num);
		for (i = 0; i < 16; i++) {
			struct h264_ref *r = &c->dpb[i];
			struct v4l2_h264_dpb_entry *e = &dp->dpb[i];

			if (!r->used)
				continue;
			e->reference_ts = r->ts;
			e->frame_num = r->frame_num;
			e->pic_num = r->long_term ? (uint32_t)r->long_idx : (uint32_t)r->frame_num_wrap;
			e->fields = V4L2_H264_FRAME_REF;
			e->top_field_order_cnt = r->top;
			e->bottom_field_order_cnt = r->bottom;
			e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID |
			    (r->is_ref ? V4L2_H264_DPB_ENTRY_FLAG_ACTIVE : 0) |
			    (r->long_term ? V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0);
		}
		out->ts = ts;
		out->poc = top < bot ? top : bot;
		out->idr = first.idr;
		out->is_ref = first.nal_ref_idc != 0;
		/* scaling matrices: the PPS's if it has them, else the SPS's */
		if (pps->scaling_present || sps->scaling_present) {
			out->has_sm = 1;
			if (pps->scaling_present) {
				h264_copy(out->sm.scaling_list_4x4, pps->sl4, sizeof(pps->sl4));
				h264_copy(out->sm.scaling_list_8x8, pps->sl8, sizeof(pps->sl8));
			} else {
				h264_copy(out->sm.scaling_list_4x4, sps->sl4, sizeof(sps->sl4));
				h264_copy(out->sm.scaling_list_8x8, sps->sl8, sizeof(sps->sl8));
			}
		}
		/* IDR: refs are gone before the picture; the controls above kept the old DPB
		 * entries (harmless: the slice lists are empty for I). Now store the picture. */
		if (first.nal_ref_idc != 0 || 1) {
			struct h264_ref cur;

			h264_zero(&cur, sizeof(cur));
			cur.used = 1;
			cur.frame_num = first.frame_num;
			cur.poc = top < bot ? top : bot;
			cur.top = top;
			cur.bottom = bot;
			cur.ts = ts;
			cur.order = c->norder++;
			if (first.nal_ref_idc != 0)
				h264_mark(c, sps, &first, &cur);
			if (first.idr)
				for (i = 0; i < 16; i++)
					if (!c->dpb[i].is_ref)
						c->dpb[i].used = 0;
			slot = h264_free_slot(c);
			c->dpb[slot] = cur;
			/* drop entries no longer referenced (we keep no output queue) */
			for (i = 0; i < 16; i++)
				if (i != slot && c->dpb[i].used && !c->dpb[i].is_ref)
					c->dpb[i].used = 0;
			if (first.nal_ref_idc == 0)
				c->dpb[slot].used = 0;	/* a non-reference picture never needs to stay */
		}
	}
	return (1);
}

#endif /* _H264CTL_H_ */
