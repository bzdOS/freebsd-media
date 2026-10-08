# M001: reset state of the A64 video engine (2026-10-05)

Procedure: guest FreeBSD 15.1 on the Banana Pi M64, `cedrus.ko` probe
(fbsd-media commit 18007e3). pll_ve gated on by the probe (PLL_VE_CTRL
0x03006207 -> 0x93006207), VE clocks enabled, reset deasserted, no SRAM claim.
`sysctl dev.cedrus.0.dump=1` after attach.

Result, VE registers 0x00-0xff (all other words read 0):

    000: c0000007 00000300 00000000 00000000
    010: 00000800 00000010 00000000 00010000
    0f0: 16890040 00000000 00000000 00000000

Facts:
- F:M001-a  Without PLL_VE enabled, the first read of any VE register hangs the
  bus (the guest stops; the hypervisor watchdog resets the board). With PLL_VE
  locked the block answers.
- F:M001-b  0x0f0 (MACC_VE_VERSION in the wiki) reads 0x16890040: the wiki's
  "VE1689" column for the A64. Upper half 0x1689, lower half 0x0040.
- F:M001-c  0x000 (MACC_VE_CTRL) reads 0xc0000007: engine field 0x7 (the wiki's
  "in reset" value) plus bits 31:30 set.
- F:M001-d  Register access does not need the SRAM C1 claim.
