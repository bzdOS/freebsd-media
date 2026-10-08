# M010: programming intra pictures of any size (2026-10-05)

Follows M009 (the lab buffer is now uncached, so results are trustworthy).
Procedure: vectors/ve_ilib.py. ffmpeg encodes an all-intra MPEG-2 stream of a
noise picture (unique pixels, so every output chunk can be located), ffmpeg decodes
the reference, the stream's own headers give the engine's header register, the lab
runs the script of M006 with the changes below, output is compared with the
reference (tolerance 2; the engine and ffmpeg differ by at most 1 per sample).
Result for every case below: status 0xc001, 0x1c4 = 0, Y and UV equal within the
tolerance (e.g. 352x288: Y 101376/101376, C 50688/50688).

- F:M010-a  0x100 (picture header) is the picture coding extension laid out as a
  word: picture_coding_type << 28 | f_code[0][0] << 24 | f_code[0][1] << 20 |
  f_code[1][0] << 16 | f_code[1][1] << 12 | intra_dc_precision << 10 |
  picture_structure << 8 | top_field_first << 7 | frame_pred_frame_dct << 6 |
  concealment_motion_vectors << 5 | q_scale_type << 4 | intra_vlc_format << 3 |
  alternate_scan << 2. This reproduces 0x1ffff340 (M006) and decodes correctly with
  intra_dc_precision 0..3, alternate_scan, intra_vlc_format, q_scale_type 1,
  interlaced frame pictures (frame_pred_frame_dct 0, top_field_first 0/1), and all of
  them together (0x1ffffb9c). Bits 1:0 are zero in every case.
- F:M010-b  0x108 = (width in macroblocks << 8) | height in macroblocks (the other
  order gives 0x1c4 = 1 on 176x144). 0x10c = pixel size of the macroblock-aligned
  picture; for intra pictures the order of the two halves made no difference (but
  see F:M012-b: width goes in the upper half).
- F:M010-c  Output layout is set by 0x0c8 (documented in the wiki as luma stride in
  15:0, chroma stride in 31:16). With 0x0ec = 0x40 the output is linear NV12: luma
  rows at the luma stride, chroma as interleaved Cb,Cr rows after the luma plane at
  the address given in 0x14c/0x15c, line pitch equal to the luma stride, and the
  chroma-stride half of 0x0c8 = luma stride / 2 (0x20 for 64 in M006). Other values
  of that half (stride, stride/4, 0) give a wrong chroma plane.
- F:M010-d  Strides 176, 192 and 256 work; 200 and 264 do not (rows land wrong).
  Use a multiple of 16 (the driver uses a multiple of 32). A picture whose width or
  height is not a multiple of 16 is decoded as whole macroblocks: the buffer needs
  the stride x (height rounded up to 16) of luma plus half of that for chroma
  (100x60 decodes with stride 112, 64 rows; the cropped 100x60 area is exact).
- F:M010-e  0x0c4 (chroma offset of a second plane) does not matter for NV12
  (0, 0x400 and 0x1234 give the same picture).
- F:M010-f  Quantiser tables: the matrices from the stream, in stream (zigzag)
  order, written through 0x180 as in M006-c (intra 0x4000 | index << 8 | value, then
  non-intra index << 8 | value). A custom intra matrix from the sequence header
  decodes correctly. The default matrices are the MPEG-2 defaults.
- F:M010-g  Slice data starts at the first slice start code (00 00 01 01..af);
  everything before it (sequence/picture headers) is not given to the engine.
  Length in 0x130 is in bits; 0x134 = source address + length.
Open: interrupt enable and acknowledge (wiki: 0x114 bits 3/4/5 enable finish,
error, VLD-request interrupts; 0x11c bits 0..2 are the flags, write 1 to clear),
P and B pictures.
