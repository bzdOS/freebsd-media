# M006: register trace of the Linux stateless driver (2026-10-05)

Method: hardware observation. The hypervisor (vetrap.c, HV_VETRAP=2) makes the
video engine page, the system-control page, the CCU (offsets below 0x400) and
the DRAM controller page invalid in stage 2, performs each guest access itself
and logs offset, direction, width and value. A mainline Linux 6.12 guest (its
own V4L2 stateless driver, built unmodified, minimal config, initramfs init
issuing one MPEG-2 request for vectors/i64.m2v slice data) ran on the board.
No driver source was read. Full trace: linux-trace-mpeg2-i64.txt.
Rig: microkernel/linux-guest/bpi_m64_trace.dts, HV_LINUX_GUEST=1, vetrace.py.

- F:M006-a  Before the engine was used, the system-control register at offset 0
  (0x01c00000) was read (0) and written 0x7fffffff. This is the missing step:
  without it the engine accepts a trigger and never touches memory; with it the
  VLD reads the stream and reports a data-dependent error (F:M006-g).
  (Earlier tests used offset 4 bit 24, which is not it.)
- F:M006-b  First VE writes, in order: 0x0ec=0x40, 0x0c4=0x400, 0x0c8=0x00200040,
  0x000=0x00130000 (not 0xc0000000).
- F:M006-c  Quantiser tables go through 0x180, one write per entry:
  intra = 0x4000 | index<<8 | value, non-intra = index<<8 | value, 64 entries each.
- F:M006-d  Then: 0x100 picture header (same value as E2), 0x108 = 0x404,
  0x10c = 0x00400040, 0x150/0x154/0x158/0x15c/0x148/0x14c = forward, back and
  reconstruction luma/chroma (all the same buffer for an I frame, chroma = luma +
  luma size), 0x12c = 0 (bit offset), 0x130 = length in bits, 0x128 = VLD address
  (flag 7 in bits 31:28, address as in M003-a), 0x134 = source address + length,
  0x110 = 0, 0x1c4 = 0, 0x1c8 = 0, 0x114 = 0x800000b8, trigger 0x118 = 0x8200000f.
  No write to 0x138, 0x13c, 0x144 for MPEG-2.
- F:M006-e  After the trigger: read 0x11c, read 0x114, write 0x114 = 0x80000080,
  write 0x11c = 7 (clear).
- F:M006-f  The addresses Linux wrote were offset by its own RAM base (the test
  guest's RAM started at 0x48000000), so that run itself ended with ERROR
  (0xc003); it is a sequence reference, not a success reference.
- F:M006-g  Replaying the sequence on the FreeBSD lab (vectors/e12-linux.lab) with
  the sram write: status 0xc003 (FINISH, ERROR), 0x1c4 = 1 or 9 depending on the
  data and on earlier runs, nothing written to the output yet. Without the sram
  write the same replay hangs with status 0xc100.
Open: what the error bits in 0x1c4 mean; whether the slice data must include the
start code, and the right bit offset; why the error differs between runs.
