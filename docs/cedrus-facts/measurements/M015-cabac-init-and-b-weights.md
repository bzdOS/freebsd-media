# M015: cabac_init_idc and explicit B weights (2026-10-06)

Method, as for M014: hardware observation. Same rig (the Linux 6.12 guest with
`HV_VETRAP=2` decodes through V4L2 stateless; the register trace comes from the vetrap
ring), same parser (`h264ctl.h`) — but here the *controls* are varied, not the stream.
The bitstream is handed to the kernel unchanged; one control value per run is
overridden by the rig (`init_h264.c`, `apply_variant()`), and a trace diff against a
control run in the same boot shows what that value becomes.

Stream: `wb.264` (x264 options `cabac=1 bframes=2 b_pyramid=0 weightb=1 weightp=2
ref=3`), 4 pictures per run, decode order I (poc 0), P (poc 6), B (poc 2), B (poc 4).
Two cycles, four entries each, 4 pictures per entry:
`traces/cabac-bw.*` = [control, cabac_init_idc 2, control, explicit B weights (both
lists weight 1)] and `traces/cabac1-bw2.*` = [control, cabac_init_idc 1, control,
explicit B weights (list 0 weight 1, list 1 weight 3)]. Offsets in the VE window;
`SRAM 0` is the weights/scratch area the driver writes through 0x2e0/0x2e4.

- F:M015-a  **`cabac_init_idc` is bits [1:0] of 0x208.** Setting it to 1 changes 0x208
  by 0x1, to 2 by 0x2, on every P and B slice; the I slice (where the syntax element
  does not exist and the rig leaves it 0) shows no change at all. M014-c listed 0x208
  as `first_mb%w << 24 | first_mb/w << 16 | nal_ref_idc << 12 | slice_type << 8 |
  direct_spatial << 2` and left bits 0..1 unaccounted; they are this field.
- F:M015-b  The noise floor of the method is exactly **bit 5 of 0x208**: the two control
  entries of a cycle differ in that one bit on every run and in nothing else. That is
  the bit M014-c already recorded as set in some of Linux's runs and not predictable
  from the controls.
- F:M015-c  **`weighted_bipred_idc` is bits [3:2] of 0x204** (as M014-c's formula
  says): 2 -> 1 changes 0x204 by 0xc. It is a PPS field, so it is written for every
  slice type, including I.
- F:M015-d  With `weighted_bipred_idc = 1` and a B slice the driver writes **0x210**
  (the two denominators) and **192 words of SRAM 0**; with `2` (implicit) it writes
  neither for B slices. The B-side programming is therefore gated by that PPS field
  alone — `V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED()` in the UAPI is exactly the condition
  the kernel implements.
- F:M015-e  The 192 words are **two 96-word halves: list 0 = words 0..95, list 1 =
  words 96..191**. Measured with the halves made distinguishable (list 0 weights 1,
  list 1 weights 3, denominators 0): words 0..95 all 1, words 96..191 all 3. This turns
  M014-g's "the list 1 half is assumed at +96 words" into a measurement, and matches the
  layout the driver already uses (`wt[96 * j + ...]`).
- F:M015-f  The decoded pictures agree with the programming: a control run is
  bit-identical to another control run (FNV of luma, all four pictures, both cycles);
  `cabac_init_idc` 1 and 2 change every P and B picture and leave the I picture
  identical (the engine does consume the field — with the wrong value the CABAC context
  init does not match the encoder); neutral explicit weights leave the first B picture
  identical and change the second B and the P picture.
- F:M015-g  In the control runs the **P** picture writes its own explicit weights
  (weightp=2, F:M014-g) and the B pictures write none; the variant only adds the B-side
  programming. The weight table is written *after* the slice-header skip and before the
  slice registers, and 0x210 is written before the table (order as in the trace).

Consequence for the driver (`fbsd-media/sys/dev/cedrus/cedrus_h264.c`): the slice
register 0x208 was written without `cabac_init_idc`, i.e. bits [1:0] zero, so a
conformant stream whose CABAC slices carry a non-zero value would be decoded with the
wrong context init. Fixed by adding the field (F:M015-a); the weight layout needed no
change, its comment now cites F:M015-e. The fix is committed in `fbsd-media` `26e72f5`
together with the rest of the H.264 decoder and its test.

Verified on the board with that build (`dmabuf_base.ko` + `media.ko` + `cedrus.ko`,
guest 15.1-RC3): `smb.264` (15 slices per picture) and `wb.264` (1 slice, I/P/B) decode
bit-exactly in every run, Y 25344/25344 and C 12672/12672 per picture. `f3.264` exposed
a second, unrelated defect on the way: the engine's own buffers are allocated with
`contigmalloc(M_NOWAIT)` inside a narrow window, which the V4L2 buffers fragment, so
slices were rejected with ENOMEM (5/10 runs at a 64 MiB window, 1/10 at 256, 0/10 at
128 and 512). The allocation now retries progressively wider windows up to the one the
data buffers use — 1/30 runs failing after, and that remainder is a different signature:
PARTIAL pictures (Y ~8400/25344) with no rejected slice, which points at capture-buffer
coherency in the media core's buffer path rather than at the slice programming.

Open: `cabac_init_idc = 3` is not a legal value (0..2), so the field's width is
inferred from two measured values plus the register layout, not from a third
measurement; explicit weights with a denominator > 0, with non-zero offsets, or with
more than one active reference per list were not measured; and no stream with a
non-zero `cabac_init_idc` exists to exercise the new driver line end to end (x264 never
writes one), so that line is verified by the register measurement above, not by a
bit-exact decode.

Artifacts (sha256):
- `traces/cabac-bw.trace.txt` 129472 B `391edc039a861f80238273b092f462a1b82066cb4590f9d10eb2ea489f513b88`
- `traces/cabac-bw.con.txt` 11950 B `ab4311c33dc06b74786e3d2131e4ce4a5e869be7095e5812a1a722dab7d366cc`
- `traces/cabac1-bw2.trace.txt` 129472 B `c39d610c0c679ed81cb74edca2d00a3514910e57867d71d1f0250166e8c653fd`
- `traces/cabac1-bw2.con.txt` 11950 B `d56868414ba8c24d0a375c307cbcdd5ff705575fb5771547d72884dfa2126a83`
- `init-h264v` (cycle 1 rig) 68544 B `db0213cea8362d6bf3cafb1051a7bde2cd3e768fa41c8c7bf67149d944e79ff4`
- `init-h264v2` (cycle 2 rig) 68544 B `87b108d2d18217a6ab110c0fd2d49ae2d8aacde14b59a3de4974a7e9c46bc4ef`
- `run-cycle.sh` 3620 B `cb2aa497ea28148e97a5ee807456d99a5a8301d32f84405276088f439f829c77`
- `analyze.py` 3747 B `bad13445f697474e4b8c4c50b4cfee23db3a2962b8235c423669ebdbb226a786`
- `init_h264.c` 11483 B `eeb05b2711e3d0611fc19de03a80dd7a62ed15bc201bfb7dfec8d381c0b2bdda`
- `mkhdr.py` 895 B `0a2b2c53fc29a655aecdb6d37afb6ca230b2d74452b3a5b53aa71a825209ebb2`
- `streams/wb.264` `dd84a2353eb2496e0e0119268642eec9b7d90a1a7105e96b7bbff5879b602d77`
