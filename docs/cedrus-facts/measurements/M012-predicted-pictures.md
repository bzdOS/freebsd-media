# M012: P and B pictures (2026-10-05)

Follows M010/M011. Procedure: vectors/ve_ilib.py (`lab_multi`, `make_plan`): ffmpeg
encodes streams with P and B pictures (testsrc2, 9 frames, IPPPPP and IBBPBBPBB, also
interlaced frames, fast motion, alternate scan with intra_vlc and non-linear
quantiser, 100x60, 304x200); each picture is decoded in coded order by the lab with
the programming of M010/M011, reference buffers assigned as below; every output
frame is compared with ffmpeg's decode of the same display position. Then the same
streams through the V4L2 driver (tests/cedrus_mpeg2.c -a).

- F:M012-a  0x100 for P and B pictures is the same word as F:M010-a with the f_codes
  of the picture coding extension (forward horizontal/vertical, backward
  horizontal/vertical) in 27:12 and picture_coding_type 2 (P) or 3 (B) in 30:28.
- F:M012-b  0x10c = (width << 16) | height of the macroblock-aligned picture,
  width in the upper half. M010-b found the order irrelevant: that held for intra
  pictures only. With height and width swapped, the macroblocks right of x = height
  (columns 9 and 10 of 11 at 176x144) of every P picture were wrong: the engine
  limits motion compensation to the size it was given.
- F:M012-c  No other register differs. The forward reference luma/chroma go to
  0x150/0x154, the backward reference to 0x158/0x15c, the new picture to 0x148/0x14c
  (all as in M006-d). P: forward = backward = the previous reference picture. B:
  forward = the older reference, backward = the newer one (stream order I B B P:
  I forward, P backward). The trigger, 0x114 and the quantiser tables are the same
  as for I. Reference pictures are plain NV12 frames with the output stride, read
  straight from the buffers that hold them.
- F:M012-d  Result: I, P and B pictures decode exactly (within 2 per sample, in
  practice identical to ffmpeg except rounding) in all of: 176x144 progressive,
  fast motion (f_codes up to 2), interlaced frame pictures (field DCT and field
  prediction in frame pictures), alternate scan + intra_vlc + non-linear quantiser +
  intra_dc_precision 2, 100x60 (cropped), 304x200. 58 of 58 pictures through the
  V4L2 driver, no timeouts, no missing references.
Open: field pictures (picture_structure 1 and 2; ffmpeg's encoder does not write
them, so there is no test stream yet), concealment motion vectors, MPEG-1.
