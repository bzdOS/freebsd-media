# M003: addressing, trigger nibbles, counters (2026-10-05)

Procedures: vectors/e5-search2.lab, e6-sweep.lab, e7-counters.lab, e8-order.lab.

- F:M003-a  The VLD address field layout in the wiki (flag 31:28, address bits
  27:4, address bits 31:28 in 3:0) is read as relative to the DRAM base
  (0x40000000): the lab buffer at pa 0x4a800000 is addressed as 0x0a800000. The
  DMA window of 256 MiB from the DRAM base is consistent with this. Not yet
  proven by a successful read.
- F:M003-b  Trigger register 0x118, low nibble, effect on STATUS 0x11c (other
  fields 0): 8 -> bit 27 set; a,b,c,d -> bit 23 set; e -> bit 0 (FINISH); f ->
  bit 8 (VLD busy) and it stays set; 1-7, 9 -> nothing. The status bits do not
  clear by writing 1s to them (0x0800000f) between runs; engine reset does.
- F:M003-c  Cycle counter 0x008: reads 0 and does not count until written
  (write bit 31 set); then counts at ~300 MHz (3.0e6 per 10 ms).
- F:M003-d  Counters 0x020/0x024 (DRAM read/write): stay 0x80000000 after the same
  enable write; no DRAM traffic is seen in any experiment so far.
- F:M003-e  VE_RESET 0x004: after writing 1 reads 0x201, after writing 0 reads 0x300.
- F:M003-f  Start-code search (nibble 8) returns in 0x1f0 exactly the offset
  written to 0x12c, whatever the buffer holds (0xff with a start code at bit 800;
  the real stream). The VLD does not appear to see the memory.
- F:M003-g  Decode trigger (0x0200100e, MB_NUM 16) sets FINISH at once, writes
  nothing to the MB header, DC/AC, NCF or reconstruction buffers.
- F:M003-h  Not the cause: SRAM C1 claim bit (SRAM_CTL1 bit 24 set or clear),
  VE_CTRL memory-type bits 17:16, order of the 0x128/0x12c/0x130 writes, CPU
  cache (the lab buffer is mapped uncached and flushed at allocation).
- F:M003-i  MBUS clock/reset are on (CCU 0x15c = 0x81000002, 0xfc bit 31 set).

Open: why the engine never touches DRAM. Next hypotheses: a required
initialisation write somewhere in 0x000-0x0ff (the wiki lists IPD/DBLK buffer
and output-format registers); the engine reading through a path that needs
another gate (not the clocks listed); the status/trigger protocol needing a
different sequence than "program everything, then trigger".
