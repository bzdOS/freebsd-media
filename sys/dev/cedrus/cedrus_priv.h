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
#include <sys/sx.h>
#include <sys/taskqueue.h>
#include <machine/bus.h>
#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>

#include <dev/media/media.h>
#include <dev/media/vm2m.h>

#include <dev/cedrus/cedrus_soc.h>
#include <dev/cedrus/cedrus_addr.h>

/*
 * The engine's address 0 and the span above it that buffers may use come from
 * the SoC table (cedrus_soc.h), the loader tunables hw.cedrus.dram_base /
 * hw.cedrus.dma_window_mb (experiments) and the memory the machine actually
 * has (cedrus_dma_setup in cedrus.c).  One engine per machine: plain globals.
 */
extern uint64_t cedrus_dram_base;	/* the engine's address 0 (physical) */
extern uint64_t cedrus_dma_window;	/* bytes above it the address field reaches */
extern uint64_t cedrus_mem_lo, cedrus_mem_hi;	/* the RAM inside that window: [lo, hi) */

MALLOC_DECLARE(M_CEDRUS);

struct cedrus_dec;

struct cedrus_softc {
	device_t	dev;
	const struct cedrus_soc *soc;	/* what the device tree said this is */
	uint64_t	ccu_pa, syscon_pa;	/* from the device tree, else the SoC table */
	bool		pll_gate;	/* the driver gates pll_ve itself */
	struct resource	*mem;
	clk_t		clk_ahb, clk_mod, clk_ram;
	hwreset_t	rst;
	int		dump;
	int		sram_claim;
	struct cdev	*lab;
	void		*lab_buf;
	size_t		lab_size;
	struct cedrus_dec *dec;		/* the V4L2 decoder, NULL if not set up */
	/* power: the engine is gated when nobody holds a reference */
	struct sx	plock;
	u_int		prefs;
	bool		powered, nopm;
	struct callout	idle;
	struct task	idle_task;
};

/* Hold the engine powered while its registers are touched / let go (may sleep). */
void cedrus_power_get(struct cedrus_softc *);
void cedrus_power_put(struct cedrus_softc *);
/* Assert and release the module reset. */
void cedrus_engine_reset(struct cedrus_softc *);
int cedrus_dec_attach(struct cedrus_softc *);
void cedrus_dec_detach(struct cedrus_softc *);

/* The reserved buffer pool (cedrus_pool.c, F:M041). */
void cedrus_pool_init(device_t);
void cedrus_pool_fini(void);
bool cedrus_pool_busy(void);
bool cedrus_pool_owns(const void *);
void *cedrus_pool_alloc(size_t, bus_addr_t *);
void cedrus_pool_free(void *, size_t);

#endif /* _DEV_CEDRUS_PRIV_H_ */
