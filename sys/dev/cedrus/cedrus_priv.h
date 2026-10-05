/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Allwinner A64 video engine ("Cedrus"): what the bring-up code (cedrus.c, the
 * lab) and the V4L2 decoder (cedrus_dec.c) share.
 */
#ifndef _DEV_CEDRUS_PRIV_H_
#define	_DEV_CEDRUS_PRIV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>

#include <dev/media/media.h>
#include <dev/media/vm2m.h>

#define	DRAM_BASE	0x40000000UL	/* engine addresses are relative to this (F:M003-a) */
#define	DMA_WINDOW	(256UL << 20)	/* the engine reaches this much DRAM (F:assessment) */

MALLOC_DECLARE(M_CEDRUS);

struct cedrus_dec;

struct cedrus_softc {
	device_t	dev;
	struct resource	*mem;
	clk_t		clk_ahb, clk_mod, clk_ram;
	hwreset_t	rst;
	int		dump;
	int		sram_claim;
	struct cdev	*lab;
	void		*lab_buf;
	size_t		lab_size;
	struct cedrus_dec *dec;		/* the V4L2 decoder, NULL if not set up */
};

/* Assert and release the module reset. */
void cedrus_engine_reset(struct cedrus_softc *);
int cedrus_dec_attach(struct cedrus_softc *);
void cedrus_dec_detach(struct cedrus_softc *);

#endif /* _DEV_CEDRUS_PRIV_H_ */
