/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cedrus: the engine's address arithmetic, free of kernel dependencies so a
 * user-space test (tools/cedrus-addr-test.c) compiles the very same code.
 *
 * The engine addresses DRAM as (physical address - dram_base), 32 bits wide
 * (F:M003-a).  In the VLD address word the 32 bits are split: address bits
 * 27:4 stay in 27:4, address bits 31:28 move to 3:0, and bits 31:28 of the
 * word carry a flag.  Plane and table addresses are plain 32-bit values.
 */
#ifndef _DEV_CEDRUS_ADDR_H_
#define	_DEV_CEDRUS_ADDR_H_

#include <sys/types.h>

/* The engine's own address of physical address pa. */
static inline uint32_t
cedrus_addr_rel(uint64_t dram_base, uint64_t pa)
{

	return ((uint32_t)(pa - dram_base));
}

/* The VLD address word for a (16-byte aligned) relative address. */
static inline uint32_t
cedrus_addr_vld(uint32_t rel, uint32_t flag)
{

	return ((flag << 28) | (rel & 0x0ffffff0) | ((rel >> 28) & 0xf));
}

/*
 * Whether [pa, pa + size) lies inside the span the engine may be given:
 * from dram_base up to dram_base + limit.  limit must not exceed the SoC's
 * window (the caller clamps it once).  Written to be safe against overflow.
 */
static inline int
cedrus_addr_in(uint64_t dram_base, uint64_t limit, uint64_t pa, uint64_t size)
{

	return (pa >= dram_base && size <= limit && pa - dram_base <= limit - size);
}

#endif /* _DEV_CEDRUS_ADDR_H_ */
