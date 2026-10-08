# M002: which MPEG-engine register bits are writable (2026-10-05)

Procedure: vectors/e3-stick.lab. Engine select = MPEG (VE_CTRL 0xc0000000), for
each word 0x100..0x1fc write 0xffffffff, read back, write 0, read back.
Tool: fbsd-media sys/dev/cedrus/lab/ve_lab.c over /dev/cedrus-lab.

- F:M002-a  Reads back all ones (full 32 bits RW): 104 124 128 130 134 138 13c
  160 164 168 16c 170 178 17c 1b8 1bc 1d8 1dc 1e4.
- F:M002-b  Partial masks (value read after writing 0xffffffff): 100:7fffffff
  108:00ffffff 10c:0fff0fff 110:0000ffff 114:ffff73f8 118:7f7fffbf 120:0000ffff
  144:fffffc00 148:fffffc00 14c:ffffff00 150:fffffc00 154:ffffff00 158:fffffc00
  15c:ffffff00 174:80ff1fff 180:00007fff 184:00007f3f 190:0007ffff 194:000fffff
  198..1a4:3fff1fff 1a8,1ac:7fff7fff 1b0,1b4:1fff1fff 1c0:bfffffff 1c8:0000ffff
  1cc:fffffc00 1d0:fffffff0 1d4:41f73f17 1e0:00031ffc.
  So buffer addresses 144/148/150/158 are 1 KiB aligned, chroma 14c/154/15c 256 B.
- F:M002-c  Never stick (read 0 after both writes): 12c (VLD offset), 140, 188,
  18c, 1c4, 1e8-1fc. 12c does not hold a value: it acts when written.
- F:M002-d  Writing all ones to 0x118 (trigger) starts something: STATUS (0x11c)
  then reads 0x0000c100 (bit 8, VLD busy) and stays so.
