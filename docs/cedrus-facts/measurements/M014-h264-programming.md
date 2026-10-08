# M014: H.264 (2026-10-06)

Method, as for M006: hardware observation. The hypervisor (HV_VETRAP=2) logs every
access to the video engine's registers while a mainline Linux 6.12 guest, built
unmodified, decodes H.264 through its V4L2 stateless interface; no driver source was
read. What is new is that the controls it is given are ours: `fbsd-media/sys/dev/media/
tests/h264ctl.h` is a header-only userland H.264 front end (Annex B reader, SPS/PPS
and slice header parser, POC, reference marking, list construction and modification,
the DPB, prediction weights, scaling matrices) that produces the V4L2 stateless
controls for each slice. The Linux rig (`vectors/h264/init_h264.c`, an initramfs
`/init` without libc, `linuxh264.sh`) decodes 18 + 11 + 3 small x264 streams
(`vectors/h264/streams`, ordered in `order*.txt`): intra/P/B, CAVLC/CABAC, 8x8
transform, scaling matrices, weighted prediction (explicit, implicit, with
denominators from a fade), several references (up to 8), B pyramid, multiple slices
(also slices starting inside a row), deblocking off and with offsets, constrained
intra prediction, chroma qp offsets, sizes 64x64 to 1280x720. Traces:
`linux-trace-h264-set{1,2,3}.txt` (13216 + 18607 + 7941 register accesses).

- F:M014-a  The controls are right: Linux decodes the first 8 pictures of every stream
  (191 slice decodes) and every picture is bit-identical (FNV of the luma) to
  ffmpeg's decode. H.264 decoding is normative, so identical is the bar.
- F:M014-b  The engine is programmed once per slice, from scratch, in this order
  (offsets in the VE window; `rel(x)` = physical address - 0x40000000, as in M003-a):
  1. system control 0x01c00000 = 0x7fffffff (once, F:M006-a)
  2. 0x0ec = 0x40 (linear NV12), 0x0c4 = luma plane size / 4 (6336 for 176x144;
     irrelevant for NV12 output), 0x0c8 = (chroma stride / 2) << 16 | luma stride,
     0x000 = 0x00130001 (engine select 1), 0x240 = 0
  3. 0x250 and 0x254 = rel() of two scratch buffers
  4. SRAM, write pointer 0x2e0 = 0x400, then 144 words through 0x2e4: the picture table
  5. 0x24c = table index of the current picture, 0x238 = slice data length in bits,
     0x234 = 0, 0x23c = rel(end of the slice data), 0x230 = flag 7 << 28 | address as in
     M003-a (start of the data), 0x050 = 0
  6. 0x224 = 7, then the header skip (F:M014-d)
  7. [P/B] SRAM 0x640 = list 0 (0x664 = list 1 for B) (F:M014-f); [weights] 0x210, SRAM 0
     (F:M014-g); [scaling matrices] SRAM 0x800/0x840/0x880 (F:M014-h)
  8. 0x204, 0x200, 0x208, 0x20c, 0x21c (F:M014-c)
  9. 0x228 acknowledged (write back what was read), 0x220 = 7, 0x224 = 8 (start)
  10. completion: bit 0 of 0x228 (F:M014-i); then 0x220 = 0, 0x228 = 7.
- F:M014-c  The slice registers (w, h in macroblocks; all from the controls):
  0x200 = 0x000d0000 | (w-1) << 8 | (h-1).
  0x204 = entropy_coding_mode << 15 | (num_ref_idx_l0_active-1) << 10 |
          (num_ref_idx_l1_active-1) << 5 | weighted_pred_flag << 4 |
          weighted_bipred_idc << 2 | constrained_intra_pred << 1 | transform_8x8_mode;
          the two counts are 0 for the slice types that do not have the list (I: both,
          P: list 1). cabac_init_idc: no stream with a value other than 0 exists (x264
          never writes one), its place is not known.
  0x208 = first_mb_in_slice % w << 24 | first_mb_in_slice / w << 16 | nal_ref_idc != 0
          << 12 | slice_type << 8 (P 0, B 1, I 2) | direct_spatial_mv_pred << 2.
          Bit 5 of this register is set in some of Linux's runs and in no way
          predictable from the controls; clearing it changes no decoded picture
          (replays of 11 streams, P and B).
  0x20c = 0x1000 | (num_ref_idx_l0_active-1) << 24 | (num_ref_idx_l1_active-1) << 16 |
          disable_deblocking_filter_idc << 8 (value 1 observed; 2 assumed the same) |
          (slice_alpha_c0_offset_div2 & 15) << 4 | (slice_beta_offset_div2 & 15).
  0x21c = (scaling matrix not present) << 24 | (second_chroma_qp_index_offset & 63) << 16 |
          (chroma_qp_index_offset & 63) << 8 | slice qp (26 + pic_init_qp_minus26 +
          slice_qp_delta).
- F:M014-d  The engine is told where slice_data() begins by skipping header_bit_size
  bits (counted from the start code, F:M014-j): command 0x224 = (n << 8) | 3 skips n bits
  (n <= 32); the driver writes 32-bit skips and a last one for the remainder (none if it
  is zero), waiting after each until bit 16 of 0x228 is set. The engine parses nothing
  of the slice header itself.
- F:M014-e  The picture table (SRAM 0x400, 18 entries of 8 words, 144 words, all words not
  listed 0): entry n = { top field order count, bottom field order count, 0, rel(luma),
  rel(chroma), rel(mv buffer, top), rel(mv buffer, bottom), 0 }. There is one entry for
  every picture the slice can reference and one for the current picture, whose index
  goes in 0x24c. Which index each picture gets is the driver's choice (Linux rotated
  them); the list bytes refer to the same indices. Chroma = luma + stride x height of
  the macroblock-aligned picture (linear NV12, M010-c).
- F:M014-f  Reference lists (SRAM 0x640 list 0, 0x664 list 1), one byte per entry,
  packed little endian in words: entry = 2 x table index of the picture (frames; the low
  bit is presumably the bottom-field flag). 7 entries -> 2 words. Written for P (list 0)
  and B (both). The lists are the ones the controls carry after reordering.
- F:M014-g  Explicit weights: 0x210 = chroma_log2_weight_denom << 4 | luma_log2_weight_denom,
  SRAM 0 (192 words): for list 0, words 0..31 luma (index i), words 32 + 2i (Cb) and
  33 + 2i (Cr) chroma; each word = weight (16 bits) | offset & 0x1ff << 16 (9 bits,
  two's complement); unused words 0. The list 1 half is assumed at +96 words (no stream
  with explicit B weights exists: x264 writes implicit ones for B). Implicit weights
  (weighted_bipred_idc 2) need nothing in SRAM.
- F:M014-h  Scaling matrices (only when the PPS flag says so): SRAM 0x800 = 8x8 intra Y
  (64 bytes, raster order as in the control), 0x840 = 8x8 inter Y, 0x880 = the six 4x4
  lists in the control's order (96 bytes). Bytes packed little endian in words.
- F:M014-i  Status 0x228: bit 0 = finished, bit 1 = error. Truncated data (20 and 200
  bytes), zeros and garbage after the NAL header all end with 0x...03 and the engine is
  ready for the next slice: no hang.
- F:M014-j  header_bit_size, as the V4L2 UAPI defines it (bits from the beginning of the
  slice data buffer, which starts at the start code: 8 x (start code bytes + 1) plus the
  header bits, emulation prevention bytes inside the header counted) is what Linux skips:
  the skip commands add up to it in all 191 runs. `vectors/h264/model.py`-style checks
  of the formulas of F:M014-c against all 191 runs found no difference (bit 5 of
  0x208 aside).
- F:M014-k  Buffers the engine uses besides the pictures: a motion vector buffer per
  picture (the table's words 5 and 6: top and bottom halves, w x h x 16 bytes each) and
  the two scratch buffers of 0x250/0x254. The engine writes more into the motion vector
  region of a picture than the two halves: replays with the buffers 3456 bytes apart
  (176x144) corrupt a few pixels of later B pictures, 3584 apart is exact. 4096 apart
  (Linux's spacing) is used until the real size is derived. Scratch buffers of 128 KiB
  each work at 64x64 to 176x144.
- F:M014-l  Replay on FreeBSD: `vectors/h264/hlab.py` + `rp.py` rebuild the register
  sequence of a Linux run with our own buffer layout (addresses and table remapped,
  interrupt off, everything else copied) and run it through /dev/cedrus-lab. Whole
  streams replayed this way decode bit-exactly: intra (CAVLC, CABAC, 8x8, scaling
  matrices, qp 7..37, deblocking on/off/offsets, constrained intra, chroma offsets,
  64x64 to 176x144), P (1 to 8 references, CAVLC and CABAC, weighted with denominators),
  B (spatial and temporal direct, pyramid with up to 4 references, implicit weights):
  126 pictures of 126 in 26 streams. First light: 4096 of 4096 luma and 2048 of 2048 chroma
  samples equal for the first intra picture (s64).
Open: field and MBAFF pictures; cabac_init_idc other than 0; explicit B weights; 4:2:2,
10 bit; sizes of the scratch and motion vector buffers for large pictures.
