# M008: Linux decodes cleanly, the FreeBSD lab is flaky (2026-10-05)

- F:M008-a  With RAM described at its real base (0x40000000) and dma-ranges on the
  soc node, the unmodified Linux driver decodes the i64 slice data to the end:
  status 0xc001, buffer flag without ERROR, 6144 non-zero bytes, first luma
  bytes 0x10. Register sequence identical to M006-b..e, with real addresses:
  0x128 = 0x76060000 (flag 7 | offset 0x06060000), 0x134 = source + length
  (0x06060869), luma/chroma 0x06080000/0x06081000 (chroma = luma + 0x1000),
  0x114 = 0x800000b8, trigger 0x8200000f.
- F:M008-b  The trace shows no write to the clock/reset unit during decoding.
  After Linux finished, PLL_VE was disabled (0x03006207), the module clock gate
  off (0x13c = 0), DRAM gate 0x100 off, bus gate 0x064 bit 0 and VE reset 0x2c4
  bit 0 on. (The CCU page trace was not confirmed to catch Linux's accesses.)
- F:M008-c  On FreeBSD (PLL_VE on, 0x13c = 0x80000000, DRAM gate on) the same
  sequence decodes correctly only in a fraction of runs, identical inputs,
  status 0xc001 or 0xc003 with 0x1c4 = 9 or 1. Not the cause (tested): module
  clock divider 0..7, 0x114 low byte, VBV_END value, stream padding, table
  contents, delay before trigger, cleanup writes after a run, destination
  distance, CPU caching of the lab buffer (mapping made uncached), writing all
  registers in one ioctl (improves, does not remove: about 5 of 12 clean).
Open: what makes Linux deterministic. Candidates: clock/PLL state during the run
(Linux leaves the boot defaults, FreeBSD enables PLL_VE at 297 MHz), DRAM clock
gate, bus load from the other guest cores.

## Addendum (same day)
- F:M008-d  Linux decodes i64 20 times in a row (also 185000 times in a loop) without a
  single error; its first frame equals ffmpeg's in Y 4061/4096, C 2023/2048 (rounding
  differences only), exactly the figures the lab reaches on its good runs.
- F:M008-e  During Linux's continuous decoding the clock state equals FreeBSD's:
  PLL_VE 0x93004203 (frac mode, 297 MHz), VE_CLK 0x13c = 0x80000000, DRAM gate 0x100 = 1,
  MBUS 0x15c = 0x81000002. So clocks are not the difference.
- F:M008-f  Lab on FreeBSD: after an engine reset the first decode often fails; without a
  reset between runs simple streams (flat, p4, p8, p16) decode every time; streams with more
  data (grad, i64, p64) fail deterministically at the same macroblock. Tested and not the
  cause: stream data in DRAM (verified from the hypervisor), cache attribute (now uncached
  in the kernel map too), delay between programming and trigger, VBV_END, 0x114 value, table
  write pacing.
Open: the remaining difference between the Linux and the lab environments (candidate:
run the lab's exact script under Linux through /dev/mem to separate script from system).

## Addendum 2026-10-05 (evening): what is NOT the difference
- F:M008-e  The 152 register writes of the successful Linux run (same i64 stream,
  byte-identical to vectors/s_i64.bin) are, write for write and value for value,
  the same as the lab script (only the DRAM addresses differ by buffer location).
  So the script is not the difference; the environment is.
- F:M008-f  Linux decodes the stream correctly (Y 4061/4096, C 2023/2048) also
  with the hypervisor trap off (HV_VETRAP=0), so the trap's pacing is not the
  reason Linux works.
- F:M008-g  Ruled out on FreeBSD, each tried on the failing i64 stream (1c4=9 every time):
  lab buffer below 128 MiB of the DRAM window (Linux used 0x06060000..0x06082000,
  lab was above; now 0x47c00000, same failure); PLL_VE rewritten to Linux's
  0x93004203; DRAM_CLK_REG (0xf4) set to Linux's 0x80100000; HDMI TCON1 off;
  CPU frequency 648 MHz; GPU bus gate/reset off. A snapshot of CCU, system
  control, DRAMC and PRCM pages (Linux after decode vs FreeBSD) shows only
  idle-gating differences plus DRAMC_CTL 0x14c/0x150 (07060a0f vs 06060a0f),
  DRAMC_COM 0x118 (counter-like) and DRAM_CLK_REG mux, none of which changed the result.
- Open: state not visible in those pages (VE internal state after reset, SRAM C
  use, kernel-side vs ioctl context) or something else FreeBSD does to the SoC.

- Resolved by M009: the lab buffer was mapped write-back in userland; see M009.
