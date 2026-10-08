/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * A reserved pool of physically contiguous, uncached memory for the engine's
 * buffers (F:M041).
 *
 * contigmalloc() needs a run of free physical pages; after the guest has run
 * for a while or loaded the GPU stack there is none (F:M040-d), and M_NOWAIT
 * does not wait for defragmentation. So the pool takes its memory once, when
 * the module attaches and DRAM is still unfragmented, and every V4L2 buffer
 * and every engine auxiliary buffer is carved from it afterwards: a bitmap of
 * pages, first fit. Freed runs merge with their neighbours by themselves (a
 * bitmap has no free list to coalesce), and freeing never allocates, so it
 * cannot fail. hw.cedrus.pool_mb = 0 turns it off (the old behaviour).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <machine/cpufunc.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>
#include <dev/cedrus/cedrus_dec.h>
#include <dev/cedrus/cedrus_priv.h>

#define	POOL_DEFAULT_MB	64

static struct {
	struct mtx	mtx;
	char		*kva;
	vm_paddr_t	pa;
	u_long		npages;
	u_long		used;		/* pages allocated */
	u_long		nlive;		/* allocations */
	u_long		nfail;
	u_long		nalloc;
	u_long		hint;		/* first page that may be free */
	uint64_t	*map;		/* one bit a page, set = in use */
	bool		inited;
} pool;

static u_int pool_mb;		/* what the pool actually has */

static inline bool
pg_used(u_long i)
{

	return ((pool.map[i / 64] >> (i % 64)) & 1);
}

static inline void
pg_set(u_long i, bool v)
{

	if (v)
		pool.map[i / 64] |= 1ULL << (i % 64);
	else
		pool.map[i / 64] &= ~(1ULL << (i % 64));
}

/* The longest run of free pages. Called with the lock held. */
static u_long
pool_largest(void)
{
	u_long i, run = 0, best = 0;

	for (i = 0; i < pool.npages; i++) {
		if (pg_used(i))
			run = 0;
		else if (++run > best)
			best = run;
	}
	return (best);
}

void *
cedrus_pool_alloc(size_t size, bus_addr_t *pap)
{
	u_long n, i, run, start;

	if (!pool.inited)
		return (NULL);
	n = atop(round_page(size));
	mtx_lock(&pool.mtx);
	run = 0;
	start = 0;
	for (i = pool.hint; i < pool.npages; i++) {
		if (pg_used(i)) {
			run = 0;
			continue;
		}
		if (run++ == 0)
			start = i;
		if (run == n)
			break;
	}
	if (n == 0 || run != n) {
		pool.nfail++;
		mtx_unlock(&pool.mtx);
		return (NULL);
	}
	for (i = start; i < start + n; i++)
		pg_set(i, true);
	pool.used += n;
	pool.nlive++;
	pool.nalloc++;
	mtx_unlock(&pool.mtx);
	bzero(pool.kva + ptoa(start), ptoa(n));
	*pap = pool.pa + ptoa(start);
	return (pool.kva + ptoa(start));
}

bool
cedrus_pool_owns(const void *p)
{

	return (pool.inited && (const char *)p >= pool.kva &&
	    (const char *)p < pool.kva + ptoa(pool.npages));
}

void
cedrus_pool_free(void *p, size_t size)
{
	u_long n, i, start;

	n = atop(round_page(size));
	start = atop((char *)p - pool.kva);
	mtx_lock(&pool.mtx);
	KASSERT(start + n <= pool.npages, ("cedrus pool: bad free"));
	for (i = start; i < start + n; i++) {
		KASSERT(pg_used(i), ("cedrus pool: double free of page %lu", i));
		pg_set(i, false);
	}
	pool.used -= n;
	pool.nlive--;
	if (start < pool.hint)
		pool.hint = start;
	mtx_unlock(&pool.mtx);
}

static int
pool_sysctl(SYSCTL_HANDLER_ARGS)
{
	u_long v = 0;

	if (pool.inited) {
		mtx_lock(&pool.mtx);
		switch (arg2) {
		case 0: v = pool.npages; break;
		case 1: v = pool.used; break;
		case 2: v = pool_largest(); break;
		case 3: v = pool.nlive; break;
		case 4: v = pool.nfail; break;
		case 5: v = pool.nalloc; break;
		}
		mtx_unlock(&pool.mtx);
	}
	/* pages -> KiB for the sizes */
	if (arg2 <= 2)
		v = v * (PAGE_SIZE / 1024);
	return (sysctl_handle_long(oidp, &v, 0, req));
}

/* Take the pool from the engine's window; sysctls under dev.cedrus.N. */
void
cedrus_pool_init(device_t dev)
{
	static const struct { const char *name, *desc; } oids[] = {
		{ "pool_kb", "reserved pool size, KiB" },
		{ "pool_used_kb", "pool memory in use, KiB" },
		{ "pool_largest_kb", "largest free run in the pool, KiB" },
		{ "pool_live", "allocations alive" },
		{ "pool_fails", "allocations the pool could not place" },
		{ "pool_allocs", "allocations made since attach" },
	};
	u_long mb = POOL_DEFAULT_MB, lo;
	int i;

	TUNABLE_ULONG_FETCH("hw.cedrus.pool_mb", &mb);
	if (mb > 0) {
		/* The lowest 256 MiB first (the engine surely reaches it), then wider. */
		for (lo = MIN(256UL << 20, cedrus_dma_window); ; lo = MIN(lo << 1, cedrus_dma_window)) {
			pool.kva = contigmalloc(mb << 20, M_CEDRUS, M_NOWAIT | M_ZERO,
			    cedrus_dram_base, cedrus_dram_base + lo - 1, PAGE_SIZE, 0);
			if (pool.kva != NULL || lo >= cedrus_dma_window)
				break;
		}
		if (pool.kva == NULL) {
			device_printf(dev, "no %lu MiB for the buffer pool; "
			    "buffers come from the general allocator\n", mb);
		} else {
			u_long o, sz = mb << 20;

			cpu_dcache_wbinv_range(pool.kva, sz);
			for (o = 0; o < sz; o += PAGE_SIZE)
				pmap_page_set_memattr(PHYS_TO_VM_PAGE(
				    pmap_kextract((vm_offset_t)pool.kva + o)),
				    VM_MEMATTR_UNCACHEABLE);
			(void)pmap_change_attr((vm_offset_t)pool.kva, sz,
			    VM_MEMATTR_UNCACHEABLE);
			pool.pa = pmap_kextract((vm_offset_t)pool.kva);
			pool.npages = atop(sz);
			pool.map = malloc(howmany(pool.npages, 64) * 8, M_CEDRUS,
			    M_WAITOK | M_ZERO);
			mtx_init(&pool.mtx, "cedrus pool", NULL, MTX_DEF);
			pool.used = pool.nlive = pool.nfail = pool.nalloc = pool.hint = 0;
			pool.inited = true;
			pool_mb = mb;
			device_printf(dev, "buffer pool: %lu MiB at pa %#jx\n", mb,
			    (uintmax_t)pool.pa);
		}
	}
	for (i = 0; i < nitems(oids); i++)
		SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
		    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
		    oids[i].name, CTLTYPE_ULONG | CTLFLAG_RD | CTLFLAG_MPSAFE,
		    NULL, i, pool_sysctl, "LU", oids[i].desc);
}

/* Is anything still carved from the pool? Then the module must stay. */
bool
cedrus_pool_busy(void)
{

	return (pool.inited && pool.nlive != 0);
}

void
cedrus_pool_fini(void)
{
	u_long o, sz;

	if (!pool.inited)
		return;
	sz = ptoa(pool.npages);
	for (o = 0; o < sz; o += PAGE_SIZE)
		pmap_page_set_memattr(PHYS_TO_VM_PAGE(
		    pmap_kextract((vm_offset_t)pool.kva + o)), VM_MEMATTR_WRITE_BACK);
	(void)pmap_change_attr((vm_offset_t)pool.kva, sz, VM_MEMATTR_WRITE_BACK);
	contigfree(pool.kva, sz, M_CEDRUS);
	free(pool.map, M_CEDRUS);
	mtx_destroy(&pool.mtx);
	pool.inited = false;
	pool.kva = NULL;
}
