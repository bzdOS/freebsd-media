/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cedrus: what differs from one Allwinner SoC to the next, as data.
 *
 * The driver finds its SoC by the device-tree compatible string and reads
 * everything that is a property of the chip from one `struct cedrus_soc`.
 * Adding a SoC is a table entry (cedrus_socs[] in cedrus.c) plus whatever the
 * new block really needs in the register programming; nothing else in the
 * driver names a board or a DRAM address.
 *
 * Only the A64 is implemented and measured.  The notes under "What differs"
 * are what is known or believed about the others; they are NOT measured here
 * and no entry for them exists.
 *
 * What the A64 entry encodes
 * --------------------------
 *  dram_base   The engine's address 0.  Every buffer address the driver
 *              programs is (physical address - dram_base) (F:M003-a).  This is
 *              a property of the SoC's memory map, not of the board: the A64
 *              maps DRAM at 0x40000000 whatever the DRAM size, so a board with
 *              1 GiB or 2 GiB has memory at 0x40000000.. as well.  A hypervisor
 *              guest whose memory does not start there is still addressed
 *              relative to 0x40000000 as long as IPA == PA (M004).
 *  dma_window  How far above dram_base the engine's 32-bit address field can
 *              point.  Address bits 27:4 and 31:28 are carried in the VLD
 *              address word (F:M003-a), so the field is 32 bits, i.e. 4 GiB;
 *              the A64 has at most 2 GiB of DRAM, so 2 GiB is the effective
 *              bound.  The old "256 MiB window" was a reading of the wiki's
 *              field layout (M003-a, "consistent with") and is refuted: 1 GiB
 *              decodes exactly (M018, M027-a); see M043.
 *  dma_default How far above dram_base the driver places buffers unless told
 *              otherwise (dev.cedrus.0.dma_mb).  The largest span measured
 *              exact in the corpus runs (M027-e).
 *
 * What differs on the other Allwinner video engines (unmeasured, from the
 * public sources named in the project's notes; verify on hardware first)
 * --------------------------------------------------------------------------
 *  H3/H5   The same register layout and clocks (ahb "ahb", mod "mod", ram
 *          "ram", one reset); DRAM at 0x40000000.  H5 is the A64's closest kin.
 *          The module clock and the PLL gating differ (check whether the
 *          clock driver of the SoC can gate pll_ve: CEDRUS_F_PLL_VE_UNGATED).
 *  H6      A different generation: other clock tree (bus/mod/ram plus a
 *          separate PLL and a higher module rate), other DRAM base handling
 *          (the engine takes absolute addresses: dram_base would be 0 and the
 *          window the full 4 GiB), a different SRAM claim, and register
 *          differences in the H.264 part (e.g. the picture-size and the
 *          scaling-list registers).  Needs its own register-map flags, not
 *          just a table row.
 *  A33/A23 Same family as H3 with an older SRAM arrangement.
 *
 * What stays board-specific on purpose: the lab (cedrus_lab.h, ve_lab.c) is a
 * research tool for the A64 register map and is opt-in (hw.cedrus.lab=1).
 */
#ifndef _DEV_CEDRUS_SOC_H_
#define	_DEV_CEDRUS_SOC_H_

#include <sys/types.h>

/* flags */
#define	CEDRUS_F_PLL_VE_UNGATED	0x0001	/* the clock framework cannot gate pll_ve: the driver does, in the CCU */
#define	CEDRUS_F_SYSCON_VE_EN	0x0002	/* the system-control register 0 needs the VE enable word (F:M006-a) */
#define	CEDRUS_F_MPEG2		0x0100	/* MPEG-2 at 0x100 (F:M006) */
#define	CEDRUS_F_H264		0x0200	/* H.264 at 0x200 (F:M014) */

struct cedrus_soc {
	const char	*compat;	/* device-tree compatible string */
	const char	*name;
	u_int		flags;
	uint64_t	dram_base;	/* the engine's address 0 (physical) */
	uint64_t	dma_window;	/* bytes above dram_base the engine's address field reaches */
	uint64_t	dma_default;	/* bytes above dram_base buffers are placed in by default */
	const char	*clk_ahb, *clk_mod, *clk_ram;	/* clock-names */
	/* fall-backs when the device tree does not say (it does on the A64) */
	uint64_t	ccu_pa;		/* clock controller */
	uint32_t	pll_ve_reg;	/* its PLL_VE register offset (CEDRUS_F_PLL_VE_UNGATED) */
	uint64_t	syscon_pa;	/* system control */
	uint32_t	syscon_ve_en;	/* the VE enable word at syscon+0 (CEDRUS_F_SYSCON_VE_EN) */
	uint32_t	syscon_sram_ctl;	/* the SRAM C1 mapping register (research sysctl) */
};

#endif /* _DEV_CEDRUS_SOC_H_ */
