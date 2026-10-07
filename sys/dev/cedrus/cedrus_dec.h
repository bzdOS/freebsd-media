/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cedrus V4L2 decoder: what the MPEG-2 and H.264 parts (cedrus_dec.c, cedrus_h264.c)
 * share: registers, the per-device and per-open structures, small helpers.
 */
#ifndef _DEV_CEDRUS_DEC_H_
#define	_DEV_CEDRUS_DEC_H_

#include <sys/sysctl.h>
#include <dev/cedrus/cedrus_priv.h>

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

/*
 * The H.264 side of the engine (F:M014): registers from 0x200, a table and lists in
 * the engine's SRAM, window through 0x2e0/0x2e4.
 */
#define	VE_CTRL_H264		0x00130001	/* F:M014-b */
#define	H264_PICSIZE		0x200	/* F:M014-c */
#define	H264_FLAGS		0x204
#define	H264_SLICE		0x208
#define	H264_DBF		0x20c
#define	H264_WDENOM		0x210	/* F:M014-g */
#define	H264_QP			0x21c
#define	H264_IRQEN		0x220	/* F:M014-b */
#define	H264_CMD		0x224
#define	 H264_CMD_RESET		0x7
#define	 H264_CMD_START		0x8
#define	 H264_CMD_SKIP(n)	(((n) << 8) | 3)	/* F:M014-d */
#define	H264_STATUS		0x228
#define	 H264_STATUS_FINISH	0x1	/* F:M014-i */
#define	 H264_STATUS_ERROR	0x2
#define	 H264_STATUS_FLAGS	0x7
#define	 H264_STATUS_SKIPPED	0x10000	/* always set in Linux's traces: not a completion flag (F:M023) */
#define	 H264_STATUS_VLD_BUSY	0x100	/* the bit stream reader is busy: wait for it to clear (F:M023) */
#define	H264_VLD_ADDR		0x230
#define	H264_VLD_OFFSET		0x234
#define	H264_VLD_LEN		0x238
#define	H264_VBV_END		0x23c
#define	H264_ENGINE		0x240
#define	H264_CUR		0x24c	/* table index of the current picture */
#define	H264_BUF_A		0x250	/* F:M014-k */
#define	H264_BUF_B		0x254
#define	H264_X050		0x050
#define	VE_SRAM_PTR		0x2e0
#define	VE_SRAM_DATA		0x2e4
#define	SRAM_H264_WEIGHTS	0x000	/* F:M014-g */
#define	SRAM_H264_TABLE		0x400	/* F:M014-e */
#define	SRAM_H264_LIST0		0x640	/* F:M014-f */
#define	SRAM_H264_LIST1		0x664
#define	SRAM_H264_SCALE8	0x800	/* F:M014-h: two 8x8 lists, then six 4x4 at 0x880 */
#define	SRAM_H264_SCALE4	0x880

#define	CEDRUS_MIN_DIM		16
#define	CEDRUS_MAX_DIM		2048
#define	CEDRUS_MIN_STREAM	(64U << 10)
#define	CEDRUS_MAX_STREAM	(32U << 20)
#define	CEDRUS_JOB_TIMEOUT	(hz)


/* --------------------------------------------------------------- structures */

struct cedrus_ctx;

enum { CEDRUS_MPEG2 = 0, CEDRUS_H264 = 1 };

/* A physically contiguous, uncached buffer in the engine's DMA window. */
struct cedrus_mem {
	void		*kva;
	bus_addr_t	pa;
	size_t		size;
};

int cedrus_mem_alloc(struct cedrus_mem *, size_t size);
void cedrus_mem_free(struct cedrus_mem *);

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
	int			cur_codec;	/* of the job on the hardware */
	bool			registered;
	bool			media_registered;
	bool			pm_held;	/* the running picture holds a power reference */
	u_int			njobs, nerrors, ntimeouts, nrejects, nmissing;
};

enum { CEDRUS_OUT = 0, CEDRUS_CAP = 1 };

/* The formats of one open file: OUTPUT is set by the user, CAPTURE follows. */
struct cedrus_fs {
	uint32_t		width, height;	/* coded size, set on the OUTPUT queue */
	uint32_t		out_size;	/* OUTPUT sizeimage */
	uint32_t		colorspace, ycbcr_enc, quantization, xfer_func;
	int			codec;		/* CEDRUS_MPEG2 or CEDRUS_H264: the OUTPUT format */
};

struct cedrus_h264;

/* Everything one open file has. */
struct cedrus_ctx {
	struct cedrus_dec	*dec;
	struct vm2m_ctx		m2m;
	struct vbq		out, cap;
	struct vctrl_handler	ctrls;
	struct vctrl		*c_seq, *c_pic, *c_quant;
	struct cedrus_fs	fs;
	struct cedrus_h264	*h264;		/* controls and buffers, made on first use */
	struct vctrl		*c_h264[8];
};

/* shared with the H.264 part */
uint32_t cedrus_stride(uint32_t width);
uint32_t cedrus_cap_height(uint32_t height);
uint32_t cedrus_cap_size(uint32_t width, uint32_t height);
uint32_t cedrus_rel(bus_addr_t pa);
uint32_t cedrus_vld_addr(bus_addr_t pa, uint32_t flag);
struct vbuf *cedrus_ref(struct cedrus_ctx *, uint64_t ts, struct vbuf *dst);

/* cedrus_h264.c */
int cedrus_h264_controls(struct cedrus_ctx *);
void cedrus_h264_free(struct cedrus_ctx *);
/* Read and check the controls, make the buffers the slice needs; 0 or an errno. May sleep. */
int cedrus_h264_prepare(struct cedrus_dec *, struct cedrus_ctx *, struct vbuf *src, struct vbuf *dst);
/* Program the engine and start the slice (after a successful prepare); d->mtx is held.
 * 0: running, the interrupt ends it; an errno: could not start; -1/-2: already done (polled), ok/error. */
int cedrus_h264_start(struct cedrus_dec *, struct cedrus_ctx *, struct vbuf *src, struct vbuf *dst);
int cedrus_h264_trace_sysctl(SYSCTL_HANDLER_ARGS);
int cedrus_h264_last_sysctl(SYSCTL_HANDLER_ARGS);
extern u_int cedrus_aux_mb;
extern u_int cedrus_src_mb;
extern u_int cedrus_dma_mb;	/* how many MiB of DRAM from DRAM_BASE buffers may use */
extern u_int cedrus_h264_oldskip;
extern u_int cedrus_skip_busy;
extern u_int cedrus_mv_kb;
extern u_int cedrus_scratch_kb;
extern u_int cedrus_aux_near;
/* The interrupt: nonzero if it was a finished slice (*error says how it ended). */
int cedrus_h264_irq(struct cedrus_dec *, bool *error);


#endif /* _DEV_CEDRUS_DEC_H_ */
