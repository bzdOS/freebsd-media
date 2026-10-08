/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * h264ctl.h: what a userland H.264 decoder does before it talks to a stateless V4L2
 * decoder, as a header-only library without libc. It reads an Annex B byte stream
 * and, picture by picture, produces the stateless controls of the V4L2 UAPI: the
 * SPS, the PPS, the scaling matrices, the prediction weights, the slice parameters
 * and the decode parameters with the DPB, including everything the controls ask of
 * userland: picture order counts (types 0, 1 and 2), reference marking (sliding
 * window and MMCO, frames and fields), reference list construction and modification
 * (8.2.4, frames and fields), frame_num gaps.
 *
 * Scope: frames, field pictures (PAFF) and MBAFF frames; no slice groups, no data
 * partitioning, no SP/SI slices. With c->strict set (the default of a zeroed
 * context is 0: the caller decides) 4:2:0 8 bit only is accepted; without it the
 * parser passes other chroma formats and bit depths through to the controls, which
 * is what the register-trace rig wants (it asks the hardware's own driver).
 * A picture is a frame or one field: the second field of a frame comes back as its
 * own picture with `second_field` set and the same `ts` as the first, so that both
 * are decoded into the same capture buffer.
 * Used by the Linux register-trace rig (freestanding) and by the FreeBSD test
 * (tests/cedrus_h264.c); include <linux/videodev2.h> first.
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

#define	H264_MAX_SLICES		64
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

/* One picture (a frame or a field): all its controls and slices. */
struct h264_pic {
	struct v4l2_ctrl_h264_sps		sps;
	struct v4l2_ctrl_h264_pps		pps;
	struct v4l2_ctrl_h264_scaling_matrix	sm;
	int					has_sm;		/* a non-flat matrix applies */
	struct v4l2_ctrl_h264_decode_params	dp;
	struct h264_slice_info			slice[H264_MAX_SLICES];
	unsigned				nslices;
	uint64_t				ts;		/* names this picture's capture buffer */
	int					poc;		/* display order key: the frame's POC */
	int					idr;
	int					is_ref;
	int					field_pic, bottom_field;
	int					second_field;	/* decoded into the buffer of the previous picture */
	int					mbaff;
	int					mmco5;
};

/* ------------------------------------------------------------------- state */

struct h264_sps_x {
	int valid;
	struct v4l2_ctrl_h264_sps v;
	int log2_max_frame_num, log2_max_poc_lsb, frame_mbs_only, mbaff, delta_pic_order_always_zero;
	int width_mbs, height_mbs, max_num_ref_frames, poc_type, direct_8x8, gaps;
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

/* A frame store: a frame, a complementary field pair or a single field. */
struct h264_ref {
	int used;			/* the store is live: a reference, or the frame being decoded */
	int nonexisting;		/* made for a gap in frame_num */
	int frame_num, frame_num_wrap;
	int has[2];			/* [0] top, [1] bottom field decoded */
	int poc_f[2];			/* their picture order counts */
	int mark[2];			/* 0 unused for reference, 1 short term, 2 long term */
	int long_idx;			/* LongTermFrameIdx while a field is long term */
	int is_frame;			/* coded as one frame */
	uint64_t ts;
	int order;			/* insertion order, for debugging */
};

struct h264_ctx {
	struct h264_sps_x	sps[H264_MAX_SPS];
	struct h264_pps_x	pps[H264_MAX_PPS];
	struct h264_ref		dpb[16];
	int			strict;		/* refuse what the video engine cannot do */
	int			prev_poc_msb, prev_poc_lsb;	/* type 0: previous reference picture */
	int			prev_frame_num_offset, prev_frame_num;	/* types 1 and 2: previous picture */
	int			prev_ref_frame_num;
	int			max_long_idx;			/* MaxLongTermFrameIdx, -1 for none */
	int			open_valid, open_fs, open_bottom, open_ref, open_frame_num;
	uint64_t		open_ts;
	int			have_prev;
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
	if (c->strict && (v->chroma_format_idc != 1 || v->bit_depth_luma_minus8 != 0 ||
	    v->bit_depth_chroma_minus8 != 0 || (v->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE)))
		return (-2);			/* 4:2:0, 8 bit only */
	v->log2_max_frame_num_minus4 = h264_ue(&r);
	s->log2_max_frame_num = v->log2_max_frame_num_minus4 + 4;
	v->pic_order_cnt_type = h264_ue(&r);
	s->poc_type = v->pic_order_cnt_type;
	if (v->pic_order_cnt_type == 0) {
		v->log2_max_pic_order_cnt_lsb_minus4 = h264_ue(&r);
		s->log2_max_poc_lsb = v->log2_max_pic_order_cnt_lsb_minus4 + 4;
	} else if (v->pic_order_cnt_type == 1) {
		if (h264_u(&r, 1)) {
			v->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
			s->delta_pic_order_always_zero = 1;
		}
		v->offset_for_non_ref_pic = h264_se(&r);
		v->offset_for_top_to_bottom_field = h264_se(&r);
		v->num_ref_frames_in_pic_order_cnt_cycle = h264_ue(&r);
		if (r.err || v->num_ref_frames_in_pic_order_cnt_cycle > 255)
			return (-3);
		for (i = 0; i < v->num_ref_frames_in_pic_order_cnt_cycle; i++)
			v->offset_for_ref_frame[i] = h264_se(&r);
	}
	v->max_num_ref_frames = h264_ue(&r);
	s->max_num_ref_frames = v->max_num_ref_frames;
	if (h264_u(&r, 1)) {
		v->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
		s->gaps = 1;
	}
	v->pic_width_in_mbs_minus1 = h264_ue(&r);
	v->pic_height_in_map_units_minus1 = h264_ue(&r);
	s->frame_mbs_only = h264_u(&r, 1);
	if (s->frame_mbs_only) {
		v->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	} else if (h264_u(&r, 1)) {
		v->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
		s->mbaff = 1;
	}
	if (h264_u(&r, 1))
		v->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	s->direct_8x8 = (v->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) != 0;
	s->width_mbs = v->pic_width_in_mbs_minus1 + 1;
	/* height in macroblocks of a frame: the map units are field macroblock rows when not frame_mbs_only */
	s->height_mbs = (v->pic_height_in_map_units_minus1 + 1) * (s->frame_mbs_only ? 1 : 2);
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
	int field_pic, bottom_field;
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
		for (i = 0; i <= nref[l] && i < 32; i++) {
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
	if (!s->frame_mbs_only) {
		h->field_pic = h264_u(&r, 1);
		if (h->field_pic)
			h->bottom_field = h264_u(&r, 1);
	}
	if (h->idr)
		h->idr_pic_id = h264_ue(&r);
	pos0 = r.pos;
	if (s->poc_type == 0) {
		h->poc_lsb = h264_u(&r, s->log2_max_poc_lsb);
		if ((p->v.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) && !h->field_pic)
			h->delta_poc_bottom = h264_se(&r);
	} else if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
		h->delta_poc[0] = h264_se(&r);
		if ((p->v.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) && !h->field_pic)
			h->delta_poc[1] = h264_se(&r);
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
	if (h->nref[0] > 31 || h->nref[1] > 31)
		return (-3);
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

/* 8.2.1. Needs the state left by the previous pictures; `finish` commits it. */
static void
h264_compute_poc(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, int *top, int *bot,
    int *msb_out, int *fno_out)
{
	int msb = 0, fno = 0;

	*top = *bot = 0;
	if (s->poc_type == 0) {
		int max_lsb = 1 << s->log2_max_poc_lsb, pmsb, plsb;

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
		if (!h->field_pic) {
			*top = msb + h->poc_lsb;
			*bot = *top + h->delta_poc_bottom;
		} else if (!h->bottom_field) {
			*top = msb + h->poc_lsb;
		} else {
			*bot = msb + h->poc_lsb;
		}
	} else {
		int max_fn = 1 << s->log2_max_frame_num;

		if (h->idr)
			fno = 0;
		else if (c->prev_frame_num > (int)h->frame_num)
			fno = c->prev_frame_num_offset + max_fn;
		else
			fno = c->prev_frame_num_offset;
		if (s->poc_type == 1) {
			const struct v4l2_ctrl_h264_sps *v = &s->v;
			int n = v->num_ref_frames_in_pic_order_cnt_cycle, absn = 0, expected = 0, k;

			if (n != 0)
				absn = fno + h->frame_num;
			if (h->nal_ref_idc == 0 && absn > 0)
				absn--;
			if (absn > 0) {
				int cyc = (absn - 1) / n, inc = (absn - 1) % n, per = 0;

				for (k = 0; k < n; k++)
					per += v->offset_for_ref_frame[k];
				expected = cyc * per;
				for (k = 0; k <= inc; k++)
					expected += v->offset_for_ref_frame[k];
			}
			if (h->nal_ref_idc == 0)
				expected += v->offset_for_non_ref_pic;
			if (!h->field_pic) {
				*top = expected + h->delta_poc[0];
				*bot = *top + v->offset_for_top_to_bottom_field + h->delta_poc[1];
			} else if (!h->bottom_field) {
				*top = expected + h->delta_poc[0];
			} else {
				*bot = expected + v->offset_for_top_to_bottom_field + h->delta_poc[0];
			}
		} else {
			int t = h->idr ? 0 : (h->nal_ref_idc == 0 ? 2 * (fno + (int)h->frame_num) - 1 :
			    2 * (fno + (int)h->frame_num));

			if (!h->field_pic)
				*top = *bot = t;
			else if (!h->bottom_field)
				*top = t;
			else
				*bot = t;
		}
	}
	*msb_out = msb;
	*fno_out = fno;
}

/* POC of a frame store as a reference entry: a frame or pair counts as Min, a lone field as itself. */
static int
h264_store_poc(const struct h264_ref *r)
{
	if (r->has[0] && r->has[1])
		return (r->poc_f[0] < r->poc_f[1] ? r->poc_f[0] : r->poc_f[1]);
	return (r->has[0] ? r->poc_f[0] : r->poc_f[1]);
}

static int
h264_any_mark(const struct h264_ref *r, int m)
{
	return (r->used && (r->mark[0] == m || r->mark[1] == m));
}

static void
h264_wrap_frame_nums(struct h264_ctx *c, const struct h264_sps_x *s, int cur_frame_num)
{
	int i, max_fn = 1 << s->log2_max_frame_num;

	for (i = 0; i < 16; i++)
		if (c->dpb[i].used && h264_any_mark(&c->dpb[i], 1))
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

/* A list entry: slot * 4 + fields (1 top, 2 bottom, 3 frame); -1 for none. */
#define	H264_ENT(slot, f)	((slot) * 4 + (f))

/*
 * 8.2.4.2.5: from an ordered list of frame stores make a list of fields: alternate the
 * parity starting with `par`, skip fields that are missing or not marked `mark`; when
 * one parity runs out, append the rest of the other.
 */
static int
h264_field_alt(const struct h264_ctx *c, const int *fl, int nf, int par, int mark, int *out)
{
	int p[2] = { 0, 0 }, want = par, n = 0, k;

	for (;;) {
		int q = want, o = 1 - want, found = -1;

		while (p[q] < nf) {
			const struct h264_ref *r = &c->dpb[fl[p[q]]];

			if (r->has[q] && r->mark[q] == mark) {
				found = fl[p[q]];
				p[q]++;
				break;
			}
			p[q]++;
		}
		if (found < 0) {
			/* this parity is exhausted: the rest of the other one, in order */
			while (p[o] < nf) {
				const struct h264_ref *r = &c->dpb[fl[p[o]]];

				if (r->has[o] && r->mark[o] == mark)
					out[n++] = H264_ENT(fl[p[o]], o ? 2 : 1);
				p[o]++;
			}
			break;
		}
		out[n++] = H264_ENT(found, q ? 2 : 1);
		want = o;
		(void)k;
	}
	return (n);
}

/* 8.2.4: initial lists then modification. Entries are H264_ENT codes or -1. */
static void
h264_ref_lists(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, int cur_poc,
    int list[2][34], int nlist[2])
{
	int i, l, n, key[16], sidx[16], lidx[16], ns = 0, nlg = 0, par = h->bottom_field ? 1 : 0;
	int max_fn = 1 << s->log2_max_frame_num;
	int cur_pic_num = h->field_pic ? 2 * (int)h->frame_num + 1 : (int)h->frame_num;
	int max_pic_num = h->field_pic ? 2 * max_fn : max_fn;
	int init[2][34 + 34], ninit[2] = { 0, 0 };

	for (l = 0; l < 2; l++) {
		nlist[l] = 0;
		for (i = 0; i < 34; i++)
			list[l][i] = -1;
	}
	if (h->slice_type == V4L2_H264_SLICE_TYPE_I)
		return;
	for (i = 0; i < 16; i++) {
		const struct h264_ref *r = &c->dpb[i];

		if (!r->used)
			continue;
		if (!h->field_pic) {
			if (r->mark[0] == 2 && r->mark[1] == 2 && r->has[0] && r->has[1]) {
				key[i] = r->long_idx;
				lidx[nlg++] = i;
			} else if (r->mark[0] == 1 && r->mark[1] == 1 && r->has[0] && r->has[1]) {
				sidx[ns++] = i;
			}
		} else {
			if (h264_any_mark(r, 2)) {
				key[i] = r->long_idx;
				lidx[nlg++] = i;
			}
			if (h264_any_mark(r, 1))
				sidx[ns++] = i;
		}
	}
	h264_sort(lidx, nlg, key, 0);
	if (h->slice_type == V4L2_H264_SLICE_TYPE_P || h->slice_type == V4L2_H264_SLICE_TYPE_SP) {
		for (i = 0; i < ns; i++)
			key[sidx[i]] = c->dpb[sidx[i]].frame_num_wrap;
		h264_sort(sidx, ns, key, 1);
		if (!h->field_pic) {
			n = 0;
			for (i = 0; i < ns; i++)
				init[0][n++] = H264_ENT(sidx[i], 3);
			for (i = 0; i < nlg; i++)
				init[0][n++] = H264_ENT(lidx[i], 3);
			ninit[0] = n;
		} else {
			n = h264_field_alt(c, sidx, ns, par, 1, init[0]);
			n += h264_field_alt(c, lidx, nlg, par, 2, init[0] + n);
			ninit[0] = n;
		}
	} else {
		int before[16], after[16], nb = 0, na = 0, poc[16], f0[16], f1[16];

		for (i = 0; i < ns; i++) {
			poc[sidx[i]] = h264_store_poc(&c->dpb[sidx[i]]);
			if (h->field_pic ? poc[sidx[i]] <= cur_poc : poc[sidx[i]] < cur_poc)
				before[nb++] = sidx[i];
			else
				after[na++] = sidx[i];
		}
		h264_sort(before, nb, poc, 1);
		h264_sort(after, na, poc, 0);
		n = 0;
		for (i = 0; i < nb; i++)
			f0[n++] = before[i];
		for (i = 0; i < na; i++)
			f0[n++] = after[i];
		n = 0;
		for (i = 0; i < na; i++)
			f1[n++] = after[i];
		for (i = 0; i < nb; i++)
			f1[n++] = before[i];
		if (!h->field_pic) {
			n = 0;
			for (i = 0; i < ns; i++)
				init[0][n++] = H264_ENT(f0[i], 3);
			for (i = 0; i < nlg; i++)
				init[0][n++] = H264_ENT(lidx[i], 3);
			ninit[0] = n;
			n = 0;
			for (i = 0; i < ns; i++)
				init[1][n++] = H264_ENT(f1[i], 3);
			for (i = 0; i < nlg; i++)
				init[1][n++] = H264_ENT(lidx[i], 3);
			ninit[1] = n;
		} else {
			n = h264_field_alt(c, f0, ns, par, 1, init[0]);
			n += h264_field_alt(c, lidx, nlg, par, 2, init[0] + n);
			ninit[0] = n;
			n = h264_field_alt(c, f1, ns, par, 1, init[1]);
			n += h264_field_alt(c, lidx, nlg, par, 2, init[1] + n);
			ninit[1] = n;
		}
		if (ninit[1] > 1) {
			int same = ninit[0] == ninit[1];

			for (i = 0; same && i < ninit[1]; i++)
				if (init[0][i] != init[1][i])
					same = 0;
			if (same) {
				int t = init[1][0];

				init[1][0] = init[1][1];
				init[1][1] = t;
			}
		}
	}
	/* truncate to the active size, then modify (8.2.4.3) */
	for (l = 0; l < (h->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 : 1); l++) {
		int act = h->nref[l] + 1, pred, k;

		for (i = 0; i < act + 1 && i < 34; i++)
			list[l][i] = i < ninit[l] ? init[l][i] : -1;
		pred = cur_pic_num;
		for (k = 0; k < h->mod_n[l]; k++) {
			int idc = h->mod_idc[l][k], val = h->mod_val[l][k], target = -1, c2, j, pp;

			if (idc == 0 || idc == 1) {
				int d = val + 1, no_wrap, picnum;

				if (idc == 0) {
					no_wrap = pred - d;
					if (no_wrap < 0)
						no_wrap += max_pic_num;
				} else {
					no_wrap = pred + d;
					if (no_wrap >= max_pic_num)
						no_wrap -= max_pic_num;
				}
				pred = no_wrap;
				picnum = no_wrap > cur_pic_num ? no_wrap - max_pic_num : no_wrap;
				for (j = 0; j < 16; j++) {
					const struct h264_ref *r = &c->dpb[j];

					if (!r->used)
						continue;
					if (!h->field_pic) {
						if (r->mark[0] == 1 && r->mark[1] == 1 && r->has[0] && r->has[1] &&
						    r->frame_num_wrap == picnum)
							target = H264_ENT(j, 3);
					} else {
						for (pp = 0; pp < 2; pp++)
							if (r->has[pp] && r->mark[pp] == 1 &&
							    (pp == par ? 2 * r->frame_num_wrap + 1 : 2 * r->frame_num_wrap) == picnum)
								target = H264_ENT(j, pp ? 2 : 1);
					}
				}
			} else if (idc == 2) {
				for (j = 0; j < 16; j++) {
					const struct h264_ref *r = &c->dpb[j];

					if (!r->used)
						continue;
					if (!h->field_pic) {
						if (r->mark[0] == 2 && r->mark[1] == 2 && r->has[0] && r->has[1] &&
						    r->long_idx == val)
							target = H264_ENT(j, 3);
					} else {
						for (pp = 0; pp < 2; pp++)
							if (r->has[pp] && r->mark[pp] == 2 &&
							    (pp == par ? 2 * r->long_idx + 1 : 2 * r->long_idx) == val)
								target = H264_ENT(j, pp ? 2 : 1);
					}
				}
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
		list[l][act] = -1;
	}
}

/* ---------------------------------------------------------------- marking */

/* A field of store `r` as a reference: its PicNum (8.2.4.1) as seen from a field of parity `par`. */
static int
h264_field_picnum(const struct h264_ref *r, int pp, int par)
{
	return (pp == par ? 2 * r->frame_num_wrap + 1 : 2 * r->frame_num_wrap);
}

/* Find the short-term picture with PicNum `pn`: sets *st and *fld (0, 1, 2 for a frame). */
static int
h264_find_short(struct h264_ctx *c, int field, int par, int pn, int *fld)
{
	int j, pp;

	for (j = 0; j < 16; j++) {
		const struct h264_ref *r = &c->dpb[j];

		if (!r->used)
			continue;
		if (!field) {
			if (r->mark[0] == 1 && r->mark[1] == 1 && r->has[0] && r->has[1] && r->frame_num_wrap == pn) {
				*fld = 2;
				return (j);
			}
		} else {
			for (pp = 0; pp < 2; pp++)
				if (r->has[pp] && r->mark[pp] == 1 && h264_field_picnum(r, pp, par) == pn) {
					*fld = pp;
					return (j);
				}
		}
	}
	return (-1);
}

static void
h264_unmark_long(struct h264_ref *r)
{
	if (r->mark[0] == 2)
		r->mark[0] = 0;
	if (r->mark[1] == 2)
		r->mark[1] = 0;
}

/*
 * 8.2.5: marking after a reference picture was decoded into store `cs` (its fields
 * already recorded there). `fld` is 0/1 for a field, 2 for a frame; `second` says it
 * is the second field of a frame. Returns 1 when the picture had memory management
 * control operation 5.
 */
static int
h264_mark(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h, int cs, int fld, int second)
{
	struct h264_ref *cur = &c->dpb[cs];
	int i, k, mmco5 = 0, cur_long = 0, field = fld < 2;
	int cur_pic_num = field ? 2 * (int)h->frame_num + 1 : (int)h->frame_num;
	int lo = field ? fld : 0, hi = field ? fld : 1;

	h264_wrap_frame_nums(c, s, (int)h->frame_num);
	if (h->idr) {
		for (i = 0; i < 16; i++)
			if (i != cs) {
				c->dpb[i].mark[0] = c->dpb[i].mark[1] = 0;
			}
		if (h->long_term_ref_flag) {
			cur_long = 1;
			cur->long_idx = 0;
			c->max_long_idx = 0;
		} else {
			c->max_long_idx = -1;
		}
	} else if (!h->adaptive) {
		/* 8.2.5.3, sliding window */
		if (!(second && cur->mark[1 - fld] == 1)) {
			int nshort = 0, nlong = 0, oldest = -1, best = 0x7fffffff, maxr = s->max_num_ref_frames ?
			    s->max_num_ref_frames : 1;

			for (i = 0; i < 16; i++) {
				if (i == cs || !c->dpb[i].used)
					continue;
				if (h264_any_mark(&c->dpb[i], 1)) {
					nshort++;
					if (c->dpb[i].frame_num_wrap < best) {
						best = c->dpb[i].frame_num_wrap;
						oldest = i;
					}
				} else if (h264_any_mark(&c->dpb[i], 2)) {
					nlong++;
				}
			}
			if (nshort + nlong >= maxr && nshort > 0 && oldest >= 0)
				c->dpb[oldest].mark[0] = c->dpb[oldest].mark[1] = 0;
		}
	} else {
		for (k = 0; k < h->mmco_n; k++) {
			int op = h->mmco_op[k], a = h->mmco_a[k], b = h->mmco_b[k], j, pp, tf, ts;

			switch (op) {
			case 1:
				ts = h264_find_short(c, field, fld, cur_pic_num - (a + 1), &tf);
				if (ts >= 0) {
					if (tf == 2)
						c->dpb[ts].mark[0] = c->dpb[ts].mark[1] = 0;
					else
						c->dpb[ts].mark[tf] = 0;
				}
				break;
			case 2:
				for (j = 0; j < 16; j++) {
					struct h264_ref *r = &c->dpb[j];

					if (!r->used)
						continue;
					if (!field) {
						if (r->mark[0] == 2 && r->mark[1] == 2 && r->long_idx == a)
							r->mark[0] = r->mark[1] = 0;
					} else {
						for (pp = 0; pp < 2; pp++)
							if (r->has[pp] && r->mark[pp] == 2 &&
							    (pp == fld ? 2 * r->long_idx + 1 : 2 * r->long_idx) == a)
								r->mark[pp] = 0;
					}
				}
				break;
			case 3:
				ts = h264_find_short(c, field, fld, cur_pic_num - (a + 1), &tf);
				if (ts < 0)
					break;
				for (j = 0; j < 16; j++)
					if (j != ts && c->dpb[j].used && h264_any_mark(&c->dpb[j], 2) &&
					    c->dpb[j].long_idx == b)
						h264_unmark_long(&c->dpb[j]);
				if (tf == 2) {
					c->dpb[ts].mark[0] = c->dpb[ts].mark[1] = 2;
				} else {
					/* the other field of this frame keeps a different index only if it was long term */
					if (c->dpb[ts].mark[1 - tf] == 2 && c->dpb[ts].long_idx != b)
						c->dpb[ts].mark[1 - tf] = 0;
					c->dpb[ts].mark[tf] = 2;
				}
				c->dpb[ts].long_idx = b;
				break;
			case 4:
				c->max_long_idx = a - 1;
				for (j = 0; j < 16; j++)
					if (c->dpb[j].used && h264_any_mark(&c->dpb[j], 2) &&
					    c->dpb[j].long_idx > c->max_long_idx)
						h264_unmark_long(&c->dpb[j]);
				break;
			case 5:
				for (j = 0; j < 16; j++)
					c->dpb[j].mark[0] = c->dpb[j].mark[1] = 0;
				c->max_long_idx = -1;
				mmco5 = 1;
				break;
			case 6:
				for (j = 0; j < 16; j++)
					if (j != cs && c->dpb[j].used && h264_any_mark(&c->dpb[j], 2) &&
					    c->dpb[j].long_idx == b)
						h264_unmark_long(&c->dpb[j]);
				if (second && cur->mark[1 - fld] == 2 && cur->long_idx != b)
					cur->mark[1 - fld] = 0;
				cur_long = 1;
				cur->long_idx = b;
				break;
			default:
				break;
			}
		}
	}
	for (k = lo; k <= hi; k++)
		cur->mark[k] = cur_long ? 2 : 1;
	return (mmco5);
}

static int
h264_alloc_store(struct h264_ctx *c)
{
	int i, victim = -1, best = 0x7fffffff;

	for (i = 0; i < 16; i++)
		if (!c->dpb[i].used)
			break;
	if (i < 16)
		victim = i;
	else
		for (i = 0; i < 16; i++)
			if (!h264_any_mark(&c->dpb[i], 1) && !h264_any_mark(&c->dpb[i], 2) &&
			    c->dpb[i].order < best) {
				best = c->dpb[i].order;
				victim = i;
			}
	if (victim < 0)
		victim = 0;
	h264_zero(&c->dpb[victim], sizeof(c->dpb[victim]));
	c->dpb[victim].used = 1;
	c->dpb[victim].order = c->norder++;
	return (victim);
}

/* 7.4.3 frame_num gaps: make "non-existing" frames for the values that were skipped. */
static void
h264_fill_gap(struct h264_ctx *c, const struct h264_sps_x *s, const struct h264_sh *h)
{
	int max_fn = 1 << s->log2_max_frame_num, fn;

	for (fn = (c->prev_ref_frame_num + 1) % max_fn; fn != (int)h->frame_num; fn = (fn + 1) % max_fn) {
		struct h264_sh g;
		int slot;

		h264_zero(&g, sizeof(g));
		g.frame_num = fn;
		g.nal_ref_idc = 1;
		slot = h264_alloc_store(c);
		c->dpb[slot].nonexisting = 1;
		c->dpb[slot].frame_num = fn;
		c->dpb[slot].has[0] = c->dpb[slot].has[1] = 1;
		c->dpb[slot].is_frame = 1;
		(void)h264_mark(c, s, &g, slot, 2, 0);
		c->prev_ref_frame_num = fn;
		c->prev_frame_num = fn;
	}
}

/* 7.4.1.2.4: does slice `b` start a new picture after slice `a`? */
static int
h264_new_pic(const struct h264_sh *a, const struct h264_sh *b)
{
	return (a->frame_num != b->frame_num || a->pps_id != b->pps_id || a->field_pic != b->field_pic ||
	    a->bottom_field != b->bottom_field || (a->nal_ref_idc == 0) != (b->nal_ref_idc == 0) ||
	    a->poc_lsb != b->poc_lsb || a->delta_poc_bottom != b->delta_poc_bottom ||
	    a->delta_poc[0] != b->delta_poc[0] || a->delta_poc[1] != b->delta_poc[1] ||
	    a->idr != b->idr || (a->idr && a->idr_pic_id != b->idr_pic_id));
}

/* ------------------------------------------------------------ the controls */

/*
 * Parse the next picture (a frame or a field) of an Annex B stream starting at *pos.
 * Returns 1 with `out` filled (and *pos after the picture), 0 at the end of the stream,
 * negative on a stream this library does not handle. `ts` names the capture buffer the
 * picture will be decoded into; for the second field of a frame it is ignored and the
 * first field's is used (out->second_field).
 */
static int
h264_next_picture(struct h264_ctx *c, const uint8_t *buf, size_t len, size_t *pos, struct h264_pic *out,
    uint64_t ts)
{
	size_t p = *pos;
	int have = 0, err, i;
	struct h264_sh first;
	struct h264_pps_x pps_c;
	struct h264_sps_x sps_c;
	int top = 0, bot = 0, msb = 0, fno = 0, second = 0, cs = 0, fld;

	h264_zero(out, sizeof(*out));
	h264_zero(&first, sizeof(first));
	for (;;) {
		size_t sc, nal, next, end;
		int sclen, type;
		uint32_t nlen;

		/* find the next start code at or after p */
		sc = p;
		while (sc + 3 <= len && !(buf[sc] == 0 && buf[sc + 1] == 0 && buf[sc + 2] == 1))
			sc++;
		if (sc + 3 > len)
			break;
		sclen = (sc > 0 && buf[sc - 1] == 0) ? 4 : 3;
		nal = sc + 3;
		if (nal >= len)
			break;
		/* the NAL ends at the next start code (3 zero bytes also end it) */
		next = nal + 1;
		while (next + 3 <= len && !(buf[next] == 0 && buf[next + 1] == 0 &&
		    (buf[next + 2] == 1 || buf[next + 2] == 0)))
			next++;
		end = next + 3 <= len ? next : len;
		nlen = (uint32_t)(end - nal);
		type = buf[nal] & 31;
		if (have && (type == 6 || type == 7 || type == 8 || type == 9 || type == 10 || type == 11)) {
			*pos = (size_t)(sc - (sclen == 4 ? 1 : 0));
			goto done;
		}
		if (type == 7) {
			if ((err = h264_parse_sps(c, buf + nal, nlen)) != 0)
				return (err);
		} else if (type == 8) {
			if ((err = h264_parse_pps(c, buf + nal, nlen)) != 0)
				return (err);
		} else if (type == 2 || type == 3 || type == 4) {
			return (-12);		/* data partitioning */
		} else if (type == 1 || type == 5) {
			struct h264_sh h;
			struct h264_pps_x *pp;
			struct h264_sps_x *ss;

			if ((err = h264_parse_slice_header(c, buf + nal, nlen, &h, &pp, &ss)) != 0)
				return (err);
			if (h.redundant_pic_cnt > 0) {
				p = end;
				continue;		/* a redundant slice: the primary one is enough */
			}
			if (have && h264_new_pic(&first, &h)) {
				*pos = (size_t)(sc - (sclen == 4 ? 1 : 0));
				goto done;
			}
			if (!have) {
				first = h;
				pps_c = *pp;
				sps_c = *ss;
				have = 1;
				out->sps = ss->v;
				out->pps = pp->v;
				out->field_pic = h.field_pic;
				out->bottom_field = h.bottom_field;
				out->mbaff = ss->mbaff && !h.field_pic;
				/* is this the second field of the frame whose first field came last? */
				second = c->open_valid && h.field_pic && h.bottom_field != c->open_bottom &&
				    h.frame_num == (uint32_t)c->open_frame_num &&
				    (h.nal_ref_idc != 0) == (c->open_ref != 0) && !h.idr;
				if (!second) {
					/* a first field or a frame: close what was left open, fill gaps, allocate */
					int max_fn = 1 << ss->log2_max_frame_num;

					if (c->open_valid && !h264_any_mark(&c->dpb[c->open_fs], 1) &&
					    !h264_any_mark(&c->dpb[c->open_fs], 2))
						c->dpb[c->open_fs].used = 0;
					c->open_valid = 0;
					if (h.idr) {
						for (i = 0; i < 16; i++)
							if (!h264_any_mark(&c->dpb[i], 1) && !h264_any_mark(&c->dpb[i], 2))
								c->dpb[i].used = 0;
					} else if (c->have_prev && h.frame_num != (uint32_t)c->prev_ref_frame_num &&
					    h.frame_num != (uint32_t)((c->prev_ref_frame_num + 1) % max_fn)) {
						h264_wrap_frame_nums(c, ss, (int)h.frame_num);
						h264_fill_gap(c, ss, &h);
					}
					cs = h264_alloc_store(c);
				} else {
					cs = c->open_fs;
				}
				c->have_prev = 1;
				h264_compute_poc(c, ss, &h, &top, &bot, &msb, &fno);
				out->second_field = second;
				out->ts = second ? c->open_ts : ts;
			}
			if (out->nslices >= H264_MAX_SLICES)
				return (-10);
			{
				struct h264_slice_info *si = &out->slice[out->nslices];
				struct v4l2_ctrl_h264_slice_params *sp = &si->sp;
				uint32_t raw_hdr;
				int cur_poc;

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
				/* the lists of this slice, from the DPB as it stands before this picture */
				{
					int list[2][34], nlist[2], l, k;

					cur_poc = h.field_pic ? (h.bottom_field ? bot : top) : (top < bot ? top : bot);
					h264_wrap_frame_nums(c, ss, (int)h.frame_num);
					h264_ref_lists(c, ss, &h, cur_poc, list, nlist);
					for (l = 0; l < 2; l++)
						for (k = 0; k < 32; k++) {
							struct v4l2_h264_reference *ref = l == 0 ?
							    &sp->ref_pic_list0[k] : &sp->ref_pic_list1[k];

							if (k < nlist[l] && list[l][k] >= 0) {
								ref->fields = list[l][k] & 3;
								ref->index = list[l][k] >> 2;
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
	fld = first.field_pic ? first.bottom_field : 2;
	{
		struct v4l2_ctrl_h264_decode_params *dp = &out->dp;
		struct h264_ref *cr = &c->dpb[cs];
		int mmco5;

		dp->nal_ref_idc = first.nal_ref_idc;
		dp->frame_num = first.frame_num;
		dp->idr_pic_id = first.idr_pic_id;
		dp->pic_order_cnt_lsb = first.poc_lsb;
		dp->delta_pic_order_cnt_bottom = first.delta_poc_bottom;
		dp->delta_pic_order_cnt0 = first.delta_poc[0];
		dp->delta_pic_order_cnt1 = first.delta_poc[1];
		dp->dec_ref_pic_marking_bit_size = first.marking_bits;
		dp->pic_order_cnt_bit_size = first.poc_bits;
		if (first.idr)
			dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
		if (first.field_pic)
			dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
		if (first.field_pic && first.bottom_field)
			dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD;
		for (i = 0; i < (int)out->nslices; i++) {
			if (out->slice[i].sp.slice_type == V4L2_H264_SLICE_TYPE_P)
				dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;
			if (out->slice[i].sp.slice_type == V4L2_H264_SLICE_TYPE_B)
				dp->flags |= V4L2_H264_DECODE_PARAM_FLAG_BFRAME;
		}
		/* the picture order counts: a frame has both, a field its own, the second field both */
		if (!first.field_pic) {
			dp->top_field_order_cnt = top;
			dp->bottom_field_order_cnt = bot;
		} else if (second) {
			dp->top_field_order_cnt = first.bottom_field ? cr->poc_f[0] : top;
			dp->bottom_field_order_cnt = first.bottom_field ? bot : cr->poc_f[1];
		} else {
			dp->top_field_order_cnt = top;
			dp->bottom_field_order_cnt = bot;
		}
		/* the DPB as it stands before this picture: every store with a field used for reference */
		h264_wrap_frame_nums(c, &sps_c, (int)first.frame_num);
		for (i = 0; i < 16; i++) {
			struct h264_ref *r = &c->dpb[i];
			struct v4l2_h264_dpb_entry *e = &dp->dpb[i];

			if (!r->used || (i == cs && !second))
				continue;
			if (!h264_any_mark(r, 1) && !h264_any_mark(r, 2))
				continue;
			e->reference_ts = r->nonexisting ? 0 : r->ts;
			e->frame_num = r->frame_num;
			e->pic_num = h264_any_mark(r, 2) ? (uint32_t)r->long_idx : (uint32_t)r->frame_num_wrap;
			e->fields = (r->mark[0] ? V4L2_H264_TOP_FIELD_REF : 0) |
			    (r->mark[1] ? V4L2_H264_BOTTOM_FIELD_REF : 0);
			e->top_field_order_cnt = r->poc_f[0];
			e->bottom_field_order_cnt = r->poc_f[1];
			e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
			    (h264_any_mark(r, 2) ? V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0) |
			    (r->is_frame ? 0 : V4L2_H264_DPB_ENTRY_FLAG_FIELD);
		}
		if (first.idr)			/* an IDR picture leaves nothing to reference */
			h264_zero(dp->dpb, sizeof(dp->dpb));
		/* scaling matrices: the PPS's if it has them, else the SPS's */
		if (pps_c.scaling_present || sps_c.scaling_present) {
			out->has_sm = 1;
			/* UAPI: applies to the picture, including matrices inherited from SPS. */
			out->pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
			if (pps_c.scaling_present) {
				h264_copy(out->sm.scaling_list_4x4, pps_c.sl4, sizeof(pps_c.sl4));
				h264_copy(out->sm.scaling_list_8x8, pps_c.sl8, sizeof(pps_c.sl8));
			} else {
				h264_copy(out->sm.scaling_list_4x4, sps_c.sl4, sizeof(sps_c.sl4));
				h264_copy(out->sm.scaling_list_8x8, sps_c.sl8, sizeof(sps_c.sl8));
			}
		}
		/* now the picture joins the DPB */
		if (!first.field_pic) {
			cr->has[0] = cr->has[1] = 1;
			cr->poc_f[0] = top;
			cr->poc_f[1] = bot;
			cr->is_frame = 1;
		} else {
			cr->has[first.bottom_field] = 1;
			cr->poc_f[first.bottom_field] = first.bottom_field ? bot : top;
		}
		cr->frame_num = first.frame_num;
		cr->frame_num_wrap = first.frame_num;
		if (!second)
			cr->ts = ts;
		out->ts = cr->ts;
		out->idr = first.idr;
		out->is_ref = first.nal_ref_idc != 0;
		mmco5 = 0;
		if (first.nal_ref_idc != 0)
			mmco5 = h264_mark(c, &sps_c, &first, cs, fld, second);
		if (mmco5) {
			int t = !first.field_pic ? (top < bot ? top : bot) : (first.bottom_field ? bot : top);

			if (!first.field_pic) {
				cr->poc_f[0] -= t;
				cr->poc_f[1] -= t;
			} else {
				cr->poc_f[first.bottom_field] = 0;
			}
			cr->frame_num = 0;
			c->prev_poc_msb = 0;
			c->prev_poc_lsb = (first.field_pic && first.bottom_field) ? 0 : cr->poc_f[0];
			c->prev_frame_num_offset = 0;
			c->prev_frame_num = 0;
			c->prev_ref_frame_num = 0;
			out->mmco5 = 1;
		} else {
			if (first.nal_ref_idc != 0) {
				c->prev_poc_msb = msb;
				c->prev_poc_lsb = first.poc_lsb;
				c->prev_ref_frame_num = first.frame_num;
			}
			c->prev_frame_num_offset = fno;
			c->prev_frame_num = first.frame_num;
		}
		out->poc = !first.field_pic ? (cr->poc_f[0] < cr->poc_f[1] ? cr->poc_f[0] : cr->poc_f[1]) :
		    (second ? h264_store_poc(cr) : cr->poc_f[first.bottom_field]);
		/* open / close the frame store */
		if (first.field_pic && !second) {
			c->open_valid = 1;
			c->open_fs = cs;
			c->open_bottom = first.bottom_field;
			c->open_ref = first.nal_ref_idc != 0;
			c->open_frame_num = first.frame_num;
			c->open_ts = cr->ts;
		} else {
			c->open_valid = 0;
			if (!h264_any_mark(cr, 1) && !h264_any_mark(cr, 2))
				cr->used = 0;	/* a non-reference picture never needs to stay */
		}
		for (i = 0; i < 16; i++)
			if (i != cs && c->dpb[i].used && !h264_any_mark(&c->dpb[i], 1) && !h264_any_mark(&c->dpb[i], 2))
				c->dpb[i].used = 0;
	}
	return (1);
}

#endif /* _H264CTL_H_ */
