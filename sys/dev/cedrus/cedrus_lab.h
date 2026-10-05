/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /dev/cedrus-lab: experiment interface of the Cedrus probe. Registers are read
 * and written one word at a time from userland; one physically contiguous
 * buffer (inside the engine's DMA window) can be mmap()ed. This is how the
 * programming sequences in docs/cedrus-facts/measurements are found. It is a
 * debugging aid, not part of the decoder interface.
 */
#ifndef _DEV_CEDRUS_LAB_H_
#define	_DEV_CEDRUS_LAB_H_

#include <sys/types.h>
#include <sys/ioccom.h>

struct cedrus_lab_info {
	uint64_t	buf_pa;		/* physical address of the buffer */
	uint64_t	buf_size;
	uint64_t	dma_base;	/* DRAM base the engine's addresses are relative to (see M-notes) */
};

struct cedrus_lab_reg {
	uint32_t	off;		/* byte offset in the engine's register window */
	uint32_t	val;
};

/* Many writes in one call, back to back (the engine may care about timing). */
#define	CEDRUS_LAB_BATCH_MAX	512
struct cedrus_lab_batch {
	uint32_t		n;
	struct cedrus_lab_reg	regs[CEDRUS_LAB_BATCH_MAX];
};

#define	CEDRUS_LAB_INFO		_IOR('V', 1, struct cedrus_lab_info)
#define	CEDRUS_LAB_RD		_IOWR('V', 2, struct cedrus_lab_reg)
#define	CEDRUS_LAB_WR		_IOW('V', 3, struct cedrus_lab_reg)
#define	CEDRUS_LAB_BATCH	_IOW('V', 5, struct cedrus_lab_batch)
#define	CEDRUS_LAB_ENGINE_RESET	_IO('V', 4)	/* assert and release the module reset */

#endif
