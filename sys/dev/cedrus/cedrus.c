/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Allwinner A64 video engine ("Cedrus"): newbus driver, bring-up and lab.
 *
 * Turns the block on (bus clock, module clock, DRAM clock, reset), then sets up
 * the V4L2 decoder (cedrus_dec.c). Also the lab used to find how the engine is
 * programmed: /dev/cedrus-lab (registers and an uncached buffer, enabled by the
 * loader tunable hw.cedrus.lab=1) and a few sysctls.
 *
 * sysctl dev.cedrus.0.dump=1 prints the first registers; .sram_claim=1 sets
 * the SRAM-C1 mapping bit (see cedrus.h for what is known about it).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <machine/bus.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>
#include <machine/resource.h>

#include <dev/fdt/fdt_common.h>
#include <dev/cedrus/cedrus_lab.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/clk/clk.h>
#include <dev/hwreset/hwreset.h>
#include <dev/cedrus/cedrus_priv.h>

extern struct bus_space memmap_bus;

/*
 * pll_ve is declared in dev/clk/allwinner/ccu_a64.c without AW_CLK_HAS_GATE, so
 * clk_enable() on it does nothing and the engine is left without a clock: the
 * first register read then hangs the bus. Until that is fixed in the tree (see
 * hal/lima/patches/freebsd-src/freebsd-ccu-a64-pll-ve.patch) the driver gates
 * the PLL itself.
 */
#define	CCU_PA		0x01c20000
#define	CCU_PLL_VE	0x18
#define	PLL_ENABLE	(1U << 31)
#define	PLL_LOCK	(1U << 28)

#define	SYSCON_PA	0x01c00000
#define	SYSCON_SIZE	0x1000
#define	SRAM_CTL1	0x04
#define	SRAM_C1_BIT	(1U << 24)

#define	LAB_BUF_SIZE	(8U << 20)
#define	LAB_HIGH	0x08000000UL	/* prefer the low 128 MiB of the window */

MALLOC_DEFINE(M_CEDRUS, "cedrus", "cedrus");

static d_ioctl_t cedrus_lab_ioctl;
static d_mmap_t cedrus_lab_mmap;

static struct cdevsw cedrus_lab_cdevsw = {
	.d_version = D_VERSION,
	.d_name = "cedrus-lab",
	.d_ioctl = cedrus_lab_ioctl,
	.d_mmap = cedrus_lab_mmap,
};

void
cedrus_engine_reset(struct cedrus_softc *sc)
{

	hwreset_assert(sc->rst);
	DELAY(10);
	hwreset_deassert(sc->rst);
}

static int
cedrus_lab_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct cedrus_softc *sc = dev->si_drv1;
	struct cedrus_lab_info *in;
	struct cedrus_lab_reg *r;

	switch (cmd) {
	case CEDRUS_LAB_INFO:
		in = (struct cedrus_lab_info *)data;
		in->buf_pa = pmap_kextract((vm_offset_t)sc->lab_buf);
		in->buf_size = sc->lab_size;
		in->dma_base = DRAM_BASE;
		return (0);
	case CEDRUS_LAB_RD:
		r = (struct cedrus_lab_reg *)data;
		if (r->off >= 0x1000 || (r->off & 3) != 0)
			return (EINVAL);
		r->val = bus_read_4(sc->mem, r->off);
		return (0);
	case CEDRUS_LAB_WR:
		r = (struct cedrus_lab_reg *)data;
		if (r->off >= 0x1000 || (r->off & 3) != 0)
			return (EINVAL);
		bus_write_4(sc->mem, r->off, r->val);
		return (0);
	case CEDRUS_LAB_BATCH: {
		struct cedrus_lab_batch *b = (struct cedrus_lab_batch *)data;
		uint32_t i;

		if (b->n > CEDRUS_LAB_BATCH_MAX)
			return (EINVAL);
		for (i = 0; i < b->n; i++)
			if (b->regs[i].off >= 0x1000 || (b->regs[i].off & 3) != 0)
				return (EINVAL);
		for (i = 0; i < b->n; i++)
			bus_write_4(sc->mem, b->regs[i].off, b->regs[i].val);
		return (0);
	}
	case CEDRUS_LAB_ENGINE_RESET:
		cedrus_engine_reset(sc);
		return (0);
	}
	return (ENOTTY);
}

/* Set the memory attribute on every page of the lab buffer. */
static void
cedrus_lab_set_memattr(struct cedrus_softc *sc, vm_memattr_t ma)
{
	vm_size_t o;

	for (o = 0; o < sc->lab_size; o += PAGE_SIZE)
		pmap_page_set_memattr(PHYS_TO_VM_PAGE(pmap_kextract(
		    (vm_offset_t)(char *)sc->lab_buf + o)), ma);
}

static int
cedrus_lab_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	struct cedrus_softc *sc = dev->si_drv1;

	if (offset >= sc->lab_size)
		return (-1);
	*paddr = pmap_kextract((vm_offset_t)(char *)sc->lab_buf + offset);
	/* The engine reads and writes DRAM directly: the CPU must not cache it. */
	*memattr = VM_MEMATTR_UNCACHEABLE;
	return (0);
}

static int
cedrus_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "allwinner,sun50i-a64-video-engine"))
		return (ENXIO);
	device_set_desc(dev, "Allwinner A64 video engine");
	return (BUS_PROBE_DEFAULT);
}

static int
cedrus_dump_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct cedrus_softc *sc = arg1;
	int v = 0, error;
	u_int i;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL || v == 0)
		return (error);
	cedrus_power_get(sc);
	for (i = 0; i < 0x100; i += 16)
		device_printf(sc->dev, "%03x: %08x %08x %08x %08x\n", i,
		    bus_read_4(sc->mem, i), bus_read_4(sc->mem, i + 4),
		    bus_read_4(sc->mem, i + 8), bus_read_4(sc->mem, i + 12));
	cedrus_power_put(sc);
	return (0);
}

static int
cedrus_sram_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct cedrus_softc *sc = arg1;
	bus_space_handle_t h;
	uint32_t r;
	int v = 0, error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	/* The system-control block belongs to aw_syscon; this touches one bit of it. */
	error = bus_space_map((&memmap_bus), SYSCON_PA, SYSCON_SIZE, 0, &h);
	if (error != 0)
		return (error);
	r = bus_space_read_4((&memmap_bus), h, SRAM_CTL1);
	device_printf(sc->dev, "SRAM_CTL1 was %08x\n", r);
	if (v != 0)
		r |= SRAM_C1_BIT;
	else
		r &= ~SRAM_C1_BIT;
	bus_space_write_4((&memmap_bus), h, SRAM_CTL1, r);
	device_printf(sc->dev, "SRAM_CTL1 now %08x\n",
	    bus_space_read_4((&memmap_bus), h, SRAM_CTL1));
	bus_space_unmap((&memmap_bus), h, SYSCON_SIZE);
	sc->sram_claim = v;
	return (0);
}

/* Debug aid: dev.cedrus.0.peek = <physical address> prints the word there. */
/* Lab aid: dev.cedrus.0.sram0 = <value> writes the system-control register at
 * offset 0 (F:M006: the Linux driver wrote 0x7fffffff there before decoding). */
static int
cedrus_sram0_sysctl(SYSCTL_HANDLER_ARGS)
{
	bus_space_handle_t h;
	unsigned int v = 0;
	int error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	error = bus_space_map(&memmap_bus, 0x01c00000, 0x1000, 0, &h);
	if (error != 0)
		return (error);
	bus_space_write_4(&memmap_bus, h, 0, v);
	bus_space_unmap(&memmap_bus, h, 0x1000);
	return (0);
}

/* Lab aid: dev.cedrus.0.poke = (physical address << 32 is not possible in 64
 * bits with a 32-bit value, so): write the address to poke_addr first, then the
 * value to poke. */
static uint64_t cedrus_poke_addr;
static int
cedrus_poke_sysctl(SYSCTL_HANDLER_ARGS)
{
	bus_space_handle_t h;
	unsigned int v = 0;
	int error;

	error = sysctl_handle_int(oidp, &v, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if ((cedrus_poke_addr & 3) != 0 || (cedrus_poke_addr >> 32) != 0)
		return (EINVAL);
	error = bus_space_map(&memmap_bus, cedrus_poke_addr & ~(uint64_t)0xfff, 0x1000, 0, &h);
	if (error != 0)
		return (error);
	bus_space_write_4(&memmap_bus, h, cedrus_poke_addr & 0xfff, v);
	bus_space_unmap(&memmap_bus, h, 0x1000);
	return (0);
}

static int
cedrus_peek_sysctl(SYSCTL_HANDLER_ARGS)
{
	struct cedrus_softc *sc = arg1;
	bus_space_handle_t h;
	uint64_t pa = 0;
	int error;

	error = sysctl_handle_64(oidp, &pa, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if ((pa & 3) != 0 || (pa >> 32) != 0)
		return (EINVAL);
	error = bus_space_map(&memmap_bus, pa & ~(uint64_t)0xfff, 0x1000, 0, &h);
	if (error != 0)
		return (error);
	device_printf(sc->dev, "peek %08jx = %08x\n", (uintmax_t)pa,
	    bus_space_read_4(&memmap_bus, h, pa & 0xfff));
	bus_space_unmap(&memmap_bus, h, 0x1000);
	return (0);
}

static int
cedrus_pll_ve(device_t dev, bool on)
{
	bus_space_handle_t h;
	uint32_t r;
	int i, error;

	error = bus_space_map(&memmap_bus, CCU_PA, 0x1000, 0, &h);
	if (error != 0)
		return (error);
	r = bus_space_read_4(&memmap_bus, h, CCU_PLL_VE);
	if (on && (r & PLL_ENABLE) == 0) {
		bus_space_write_4(&memmap_bus, h, CCU_PLL_VE, r | PLL_ENABLE);
		for (i = 0; i < 1000; i++) {
			if ((bus_space_read_4(&memmap_bus, h, CCU_PLL_VE) & PLL_LOCK) != 0)
				break;
			DELAY(10);
		}
		if (i == 1000)
			error = ETIMEDOUT;
	} else if (!on) {
		bus_space_write_4(&memmap_bus, h, CCU_PLL_VE, r & ~PLL_ENABLE);
	}
	bus_space_unmap(&memmap_bus, h, 0x1000);
	return (error);
}

/* ------------------------------------------------------------------- power */

#define	CEDRUS_IDLE_TICKS	(hz / 5)	/* gate the engine this long after its last use */
#define	SYSCON_VE_ENABLE	0x7fffffff	/* F:M006-a */

static void cedrus_idle_cb(void *);

static void
cedrus_power_on_locked(struct cedrus_softc *sc)
{
	bus_space_handle_t h;

	/* Reset asserted while the clocks come up, then released. */
	hwreset_assert(sc->rst);
	if (cedrus_pll_ve(sc->dev, true) != 0)
		device_printf(sc->dev, "PLL_VE did not lock\n");
	if (clk_enable(sc->clk_ahb) != 0 || clk_enable(sc->clk_mod) != 0 ||
	    clk_enable(sc->clk_ram) != 0)
		device_printf(sc->dev, "clk_enable failed\n");
	hwreset_deassert(sc->rst);
	/* F:M006-a: the step without which the engine does not read memory. */
	if (bus_space_map(&memmap_bus, SYSCON_PA, SYSCON_SIZE, 0, &h) == 0) {
		bus_space_write_4(&memmap_bus, h, 0, SYSCON_VE_ENABLE);
		bus_space_unmap(&memmap_bus, h, SYSCON_SIZE);
	}
	sc->powered = true;
}

static void
cedrus_power_off_locked(struct cedrus_softc *sc)
{

	hwreset_assert(sc->rst);
	clk_disable(sc->clk_ram);
	clk_disable(sc->clk_mod);
	clk_disable(sc->clk_ahb);
	cedrus_pll_ve(sc->dev, false);
	sc->powered = false;
}

/*
 * The engine's registers hang the bus while its clock is off (F:M001-a), so
 * every user holds a reference while it touches them. May sleep.
 */
void
cedrus_power_get(struct cedrus_softc *sc)
{

	sx_xlock(&sc->plock);
	sc->prefs++;
	callout_stop(&sc->idle);
	if (!sc->powered)
		cedrus_power_on_locked(sc);
	sx_xunlock(&sc->plock);
}

/* Let go; the engine is gated a little later if nobody took it again. */
void
cedrus_power_put(struct cedrus_softc *sc)
{

	sx_xlock(&sc->plock);
	if (--sc->prefs == 0 && !sc->nopm)
		callout_reset(&sc->idle, CEDRUS_IDLE_TICKS, cedrus_idle_cb, sc);
	sx_xunlock(&sc->plock);
}

static void
cedrus_idle_cb(void *arg)
{
	struct cedrus_softc *sc = arg;

	taskqueue_enqueue(taskqueue_thread, &sc->idle_task);
}

static void
cedrus_idle_task(void *arg, int pending)
{
	struct cedrus_softc *sc = arg;

	sx_xlock(&sc->plock);
	if (sc->prefs == 0 && sc->powered && !sc->nopm)
		cedrus_power_off_locked(sc);
	sx_xunlock(&sc->plock);
}

static int
cedrus_attach(device_t dev)
{
	struct cedrus_softc *sc = device_get_softc(dev);
	int rid = 0, error;
	uint64_t f;

	sc->dev = dev;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "no registers\n");
		return (ENXIO);
	}
	error = clk_get_by_ofw_name(dev, 0, "ahb", &sc->clk_ahb);
	if (error == 0)
		error = clk_get_by_ofw_name(dev, 0, "mod", &sc->clk_mod);
	if (error == 0)
		error = clk_get_by_ofw_name(dev, 0, "ram", &sc->clk_ram);
	if (error == 0)
		error = hwreset_get_by_ofw_idx(dev, 0, 0, &sc->rst);
	if (error != 0) {
		device_printf(dev, "clocks/reset: %d\n", error);
		goto fail;
	}
	sx_init(&sc->plock, "cedrus power");
	callout_init(&sc->idle, 1);
	TASK_INIT(&sc->idle_task, 0, cedrus_idle_task, sc);
	{
		u_long pm = 1;

		TUNABLE_ULONG_FETCH("hw.cedrus.pm", &pm);
		sc->nopm = (pm == 0);
	}
	cedrus_power_get(sc);		/* held until the end of attach */
	clk_get_freq(sc->clk_mod, &f);
	device_printf(dev, "mod clock %ju Hz\n", (uintmax_t)f);
	clk_get_freq(sc->clk_ahb, &f);
	device_printf(dev, "ahb clock %ju Hz\n", (uintmax_t)f);

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "dump",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    cedrus_dump_sysctl, "I", "print the first registers");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "sram_claim",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    cedrus_sram_sysctl, "I", "set/clear the SRAM C1 mapping bit");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "sram0",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
	    cedrus_sram0_sysctl, "IU", "write system-control register 0");
	SYSCTL_ADD_U64(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "poke_addr",
	    CTLFLAG_RW, &cedrus_poke_addr, 0, "physical address for poke");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "poke",
	    CTLTYPE_UINT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
	    cedrus_poke_sysctl, "IU", "write a word at poke_addr");
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "peek",
	    CTLTYPE_U64 | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    cedrus_peek_sysctl, "QU", "print the word at a physical address");

	/*
	 * The lab buffer: contiguous, inside the engine's DMA window. Only when
	 * asked for (hw.cedrus.lab=1): it takes memory the decoder wants.
	 */
	{
		u_long lab = 0;

		TUNABLE_ULONG_FETCH("hw.cedrus.lab", &lab);
		sc->lab_size = LAB_BUF_SIZE;
		TUNABLE_ULONG_FETCH("hw.cedrus.lab_size", &sc->lab_size);
		if (lab != 0) {
			sc->lab_buf = contigmalloc(sc->lab_size, M_CEDRUS,
			    M_NOWAIT | M_ZERO, DRAM_BASE, DRAM_BASE + LAB_HIGH - 1,
			    PAGE_SIZE, 0);
			if (sc->lab_buf == NULL)
				sc->lab_buf = contigmalloc(sc->lab_size, M_CEDRUS,
				    M_NOWAIT | M_ZERO, DRAM_BASE,
				    DRAM_BASE + DMA_WINDOW - 1, PAGE_SIZE, 0);
		}
	}
	if (sc->lab_buf != NULL) {
		cpu_dcache_wbinv_range(sc->lab_buf, sc->lab_size);
		/* One attribute for every mapping of these pages (the engine reads and
		 * writes DRAM behind the CPU's back). The device pager takes a real
		 * page's attribute from the page itself, not from d_mmap, so the
		 * pages must carry it: otherwise user mappings come out write-back
		 * and the engine reads stale DRAM (M009). */
		cedrus_lab_set_memattr(sc, VM_MEMATTR_UNCACHEABLE);
		if (pmap_change_attr((vm_offset_t)sc->lab_buf, sc->lab_size,
		    VM_MEMATTR_UNCACHEABLE) != 0)
			device_printf(dev, "could not make the lab buffer uncached\n");
		sc->lab = make_dev_credf(MAKEDEV_CHECKNAME, &cedrus_lab_cdevsw, 0,
		    NULL, UID_ROOT, GID_WHEEL, 0600, "cedrus-lab");
		if (sc->lab != NULL)
			sc->lab->si_drv1 = sc;
		device_printf(dev, "lab buffer %zu bytes at pa %#jx\n", sc->lab_size,
		    (uintmax_t)pmap_kextract((vm_offset_t)sc->lab_buf));
	}
	error = cedrus_dec_attach(sc);
	if (error != 0)
		device_printf(dev, "V4L2 decoder: %d\n", error);
	if (sc->lab_buf != NULL)
		sc->nopm = true;	/* the lab pokes registers whenever it likes */
	cedrus_power_put(sc);
	return (0);
fail:
	bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
	return (ENXIO);
}

static int
cedrus_detach(device_t dev)
{
	struct cedrus_softc *sc = device_get_softc(dev);

	cedrus_dec_detach(sc);
	if (sc->lab != NULL)
		destroy_dev(sc->lab);
	if (sc->lab_buf != NULL) {
		cedrus_lab_set_memattr(sc, VM_MEMATTR_WRITE_BACK);
		(void)pmap_change_attr((vm_offset_t)sc->lab_buf, sc->lab_size,
		    VM_MEMATTR_WRITE_BACK);
		contigfree(sc->lab_buf, sc->lab_size, M_CEDRUS);
	}
	callout_drain(&sc->idle);
	taskqueue_drain(taskqueue_thread, &sc->idle_task);
	if (sc->powered)
		cedrus_power_off_locked(sc);
	sx_destroy(&sc->plock);
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
	return (0);
}

static device_method_t cedrus_methods[] = {
	DEVMETHOD(device_probe,		cedrus_probe),
	DEVMETHOD(device_attach,	cedrus_attach),
	DEVMETHOD(device_detach,	cedrus_detach),
	DEVMETHOD_END
};

static driver_t cedrus_driver = {
	"cedrus",
	cedrus_methods,
	sizeof(struct cedrus_softc),
};

DRIVER_MODULE(cedrus, simplebus, cedrus_driver, 0, 0);
MODULE_DEPEND(cedrus, media, 1, 1, 1);
MODULE_VERSION(cedrus, 1);
