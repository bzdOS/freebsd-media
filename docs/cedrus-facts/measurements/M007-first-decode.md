# M007: first correct hardware decode (2026-10-05)

Follows M006. Tools: vectors/ve_exp.py (builds a lab script from a stream and the
M006 sequence, runs it on the guest, compares with the ffmpeg decode),
vectors/flat.m2v, grad.m2v, i64.m2v and their .yuv references.

- F:M007-a  With the system-control write of M006-a, 0x114 = 0x80000008 and
  VBV_END (0x134) = source address + 0x1000 (any value well past the data; not
  source + length), a 64x64 all-gray I-frame (vectors/flat.m2v) decodes
  exactly: luma 4096/4096 and chroma 2048/2048 bytes equal to ffmpeg's decode,
  status 0xc001 (FINISH, no error), 0x1c4 = 0.
- F:M007-b  The output is linear NV12 (0x0ec = 0x40): luma rows at the picture
  stride (64 bytes), chroma as interleaved UV after luma + 0x1000. Not tiled.
- F:M007-c  0x108 = (width in macroblocks << 8) | height in macroblocks; with
  VBV_END = source + length the last macroblock row is not written and the
  status ends with ERROR (0x1c4 = 9). Setting the height to 3 decodes three rows
  without error.
- F:M007-d  0x114 bit 7 set stops all output; bits 6..0 change little for the
  gray stream. The Linux value 0xb8 gives no output.
- F:M007-e  Slice data must start at the first slice start code and be fed from
  macroblock 0: a single later slice gives 0x1c4 = 5.
- F:M007-f  Streams with texture do not decode fully. i64.m2v: the first two 8x8
  luma blocks are exact (ffmpeg's values), the third is garbage, 0x1c4 = 9.
  grad.m2v: first macroblock row and one macroblock of the second are right
  (rounding differs by one in a few pixels), then no output; error 9. The point
  where it stops changes with the length written to 0x130 (shorter by 64 bits
  decodes further) and with the 0x114 low byte, not with the picture header bits
  of 0x100 (except bits 6 and 13). The quantiser table writes (0x180) change
  nothing measurable, in either selector range, between runs.
Open: why textured data stops; what the 0x180 table writes do (their effect looks
independent of content); the meaning of error bits 0x1c4 (1, 5, 9, 0xd, 0x21).
