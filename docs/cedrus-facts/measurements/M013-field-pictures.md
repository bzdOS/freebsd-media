# M013: field pictures (2026-10-05)

Streams: vectors/mpeg2_fieldgen.py, a small MPEG-2 writer made for this: intra
macroblocks with random DC values and no AC (every 8x8 block a random constant, so any
mix-up of lines shows), and P macroblocks of "motion, not coded" type with zero vectors and
a random `motion_vertical_field_select` per macroblock. ffmpeg decodes them (it is the
reference; a frame P picture made the same way reproduces the I frame, which validates
the writer). Frame A: I top field + I bottom field; frames B, C: P top + P bottom, each
macroblock copying a randomly chosen field of the reference.

- F:M013-a  A field picture is programmed like a frame picture with (1) 0x100 carrying
  picture_structure 1 (top) or 2 (bottom) (bits 9:8, F:M010-a), (2) 0x108 and 0x10c
  carrying the height of the FIELD (rows of macroblocks, half the frame's), width as
  before, and (3) the addresses of the whole frame buffer, strides and chroma offset
  of the frame (F:M010-c). The engine writes the lines of a top field to rows 0, 2, 4, ...
  and of a bottom field to rows 1, 3, 5, ... of that buffer by itself. With the frame
  height in 0x108 a field picture ends with 0x1c4 = 9.
- F:M013-b  P fields: the forward reference is the frame buffer of the previous reference
  frame (0x150/0x154 as for frame pictures); a bottom field whose macroblocks select
  the top field of the reference list copies the first field of the SAME frame
  (written to the destination buffer a moment earlier), without that buffer being
  named as a reference. Exact (0 wrong samples of 92160) with random selects, with
  select all 0 and all 1.
- F:M013-c  Through V4L2 (tests/cedrus_mpeg2.c -a): both fields of a frame go into one
  capture buffer; the OUTPUT buffer of the first field carries
  V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF so the scheduler keeps the capture buffer for the
  next job, and the frame completes with the second field. 6 of 6 frames exact.
Not tested: B field pictures (the writer has no backward vectors), dual-prime and 16x8
motion, field pictures with residuals.
