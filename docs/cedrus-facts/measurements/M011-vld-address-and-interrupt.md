# M011: stream start alignment, interrupt, damaged streams (2026-10-05)

Procedure: vectors/ve_ilib.py (lab, `loadoff`/`vldoff`/`bitoff`) for the address
experiment; the V4L2 driver (fbsd-media sys/dev/cedrus/cedrus_dec.c) with
tests/cedrus_mpeg2.c for the rest, on the board, noise pictures from ffmpeg.

- F:M011-a  The VLD address (0x128) has 16-byte granularity (the low four bits of
  the word carry address bits 31:28, F:M003-a). Slice data that starts at a
  buffer address that is not a multiple of 16 is decoded without error by writing
  the address rounded down to 16 and the remainder, in bits, to 0x12c (offset 5
  -> 0x12c = 40, offset 9 -> 72), with 0x130 = the data length in bits and 0x134 =
  the end of the data. Writing the unaligned address itself finishes with
  0x1c4 = 1 (the picture is still right: the error comes at the end).
- F:M011-b  With 0x114 = 0x800000b8 (bits 3, 4, 5 enable the finish, error and
  VLD-request interrupts, wiki) the engine interrupts at the end of a picture:
  device-tree SPI 58. A handler that reads 0x11c, writes 0x114 = 0x80000080 and
  0x11c = 7 (F:M006-e) and then completes the buffers decoded 300 pictures in a
  row with no polling and no timeout. Flag bit 0 = finish, bit 1 = error; both are
  set on an error (0xc003).
- F:M011-c  Damaged input never hangs the engine: slice data cut to 5, 20, 300 and
  1000 bytes, and garbage after the first start code, each ended with
  FINISH | ERROR (10 of 10), and the next good picture decoded exactly, with no
  engine reset in between.
- F:M011-d  A lab script that enables the interrupts (0x114 bits 3..5) loses its
  status: the driver's handler clears the flags first. Lab scripts use
  0x114 = 0x80000080 (no interrupts) when the driver is loaded.
