/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Host test of the Cedrus address arithmetic (sys/dev/cedrus/cedrus_addr.h, the
 * very header the driver includes) for DRAM layouts other than the chimp guest's:
 * 1 GiB and 2 GiB boards, RAM that starts above the engine's address 0, buffers
 * at the edges of the window.  Build and run on any host:
 *
 *   cc -I sys -o /tmp/cedrus-addr-test sys/dev/media/tools/cedrus-addr-test.c && /tmp/cedrus-addr-test
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <dev/cedrus/cedrus_addr.h>

static int fails;
#define	CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* The inverse of cedrus_addr_vld, as the engine reads it (F:M003-a). */
static uint32_t
vld_decode(uint32_t w)
{

	return ((w & 0x0ffffff0) | ((w & 0xf) << 28));
}

int
main(void)
{
	const uint64_t base = 0x40000000ULL, MiB = 1ULL << 20;
	static const uint64_t dram[] = { 512 * MiB, 1024 * MiB, 1792 * MiB, 2048 * MiB };
	static const uint64_t limit[] = { 256 * MiB, 1024 * MiB, 2048 * MiB };
	uint64_t pa;
	unsigned i, j;

	/* relative addresses and the VLD word round-trip over the whole 2 GiB reach */
	for (pa = base; pa < base + 2048 * MiB; pa += 16 * 4093) {
		uint32_t rel = cedrus_addr_rel(base, pa);

		CHECK(rel == pa - base);
		CHECK(vld_decode(cedrus_addr_vld(rel, 7)) == (rel & ~0xfu));
		CHECK((cedrus_addr_vld(rel, 7) >> 28) == 7);
	}
	/* the F:M003-a layout, by hand: pa 0x4a800000 -> 0x0a800000; 0x7ff40000 -> bits 31:28 = 3 in 3:0 */
	CHECK(cedrus_addr_vld(cedrus_addr_rel(base, 0x4a800000), 0) == 0x0a800000);
	CHECK(cedrus_addr_vld(cedrus_addr_rel(base, 0x7ff40000), 0) == 0x0ff40003);
	CHECK(cedrus_addr_vld(cedrus_addr_rel(base, 0xb0000000 - 16), 0) == 0x0ffffff6);

	/* window checks for each DRAM size against each limit */
	for (i = 0; i < sizeof(dram) / sizeof(dram[0]); i++) {
		for (j = 0; j < sizeof(limit) / sizeof(limit[0]); j++) {
			uint64_t lim = limit[j] < dram[i] ? limit[j] : dram[i];	/* what is placeable */
			uint64_t sz = 4 * MiB;

			CHECK(cedrus_addr_in(base, limit[j], base, sz));
			CHECK(cedrus_addr_in(base, limit[j], base + limit[j] - sz, sz));
			CHECK(!cedrus_addr_in(base, limit[j], base + limit[j] - sz + 16, sz));
			CHECK(!cedrus_addr_in(base, limit[j], base + limit[j], sz));
			CHECK(!cedrus_addr_in(base, limit[j], base - sz, sz));	/* below the engine's address 0 */
			CHECK(!cedrus_addr_in(base, limit[j], 0, sz));
			CHECK(!cedrus_addr_in(base, limit[j], base, limit[j] + 1));
			CHECK(!cedrus_addr_in(base, limit[j], ~0ULL - 4, 16));		/* no wrap-around */
			CHECK(!cedrus_addr_in(base, limit[j], base + 16, ~0ULL));
			(void)lim;
		}
	}
	/* a machine whose RAM starts at base + 256 MiB: the engine still counts from base */
	CHECK(cedrus_addr_rel(base, base + 256 * MiB) == 0x10000000u);
	CHECK(cedrus_addr_in(base, 1024 * MiB, base + 256 * MiB, 64 * MiB));
	/* the same physical memory seen from a base that is too high is out of reach */
	CHECK(!cedrus_addr_in(base + 512 * MiB, 1024 * MiB, base + 256 * MiB, 64 * MiB));

	printf(fails == 0 ? "cedrus-addr-test: all passed\n" : "cedrus-addr-test: %d failed\n", fails);
	return (fails != 0);
}
