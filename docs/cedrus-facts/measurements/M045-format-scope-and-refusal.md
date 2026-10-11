# M045 - interlaced, 4:2:2 and high bit depth: what decodes, what is refused, and where

2026-10-11. Guest kernel FreeBSD 15.1-RC3 #26 (the WiFi build, `BPI64WIFI`), modules built
from branch `h264-interlaced` against `freebsd-src-earlyboot-wt`: cedrus.ko
`ce2c77a7eb9b29389b9c3a71a0798c44`, media.ko `955ba07a7be4c06dfcc46b77ba0cb479`,
dmabuf_base.ko `877facc5b960469094af8d66e72fb22f` (unchanged), every upload md5-checked.

## F:M045-a - the conformance inventory is complete; interlaced is already in the exact set

The local corpus (`linux-work/jvt/{AVCv1,FRExt,Professional_profiles}`) was compared with the
ITU-T directory listings of `jvt-site/draft_conformance/` on 2026-10-11: 135 + 69 + 27 = 231
archives there, all 231 present here, none missing. Of the 231, **85 have
`frame_mbs_only_flag = 0`** (field pictures, PAFF and/or MBAFF). 73 of them are 8-bit 4:2:0
and are part of the 176-stream exact set since M026-M028 (CVFI*, CVPA*, CVMA*, CAMA*, CAPAMA3,
CVSE*, CAMP_MOT_MBAFF_*, CVMP_MOT_*, cabac/cavlc_mot_{fld,mbaff,picaff}0_full, Sharp_MP_Field/PAFF,
MR6-MR9, CAPM3, HPCAFL*/HPCVFL*, brcm_freh*, ...). The other 12 interlaced streams are
10-bit (PPH10I4-7) or 10-bit 4:2:2 (Hi422FREXT16-19, PPH422I4-7): refused for depth/chroma,
not for field coding. So field pictures and MBAFF need no new work on this silicon; the full
rerun below is their regression.

## F:M045-b - full corpus rerun with the new driver: no regression

`h264-campaign/board-corpus.py --frames 0 --names <the 176 exact streams of M028
corpus-final.json>`: **176/176 streams exact, 19,028/19,028 full Y/C frames**, 0 reference warnings, 0 FAIL/ioctl/incomplete lines, 165 references equal to the official corpus YUV. Per stream, frame counts, input/reference md5s and pass counts are identical to M028 `corpus-final.json` (no difference). This includes all 73 interlaced 8-bit 4:2:0 streams. Driver counters read after the run on the same boot: pictures 24,746, errors 0, rejects 0, timeouts 0. Evidence: `linux-work/h264-traces/m045-corpus/`
(result.json, per-stream logs).

## F:M045-c - 4:2:2 and 10/14-bit H.264 are not decodable on the A64 engine

What is known, strongest first:

1. **Vendor statement via the platform maintainer** (M030): "HW doesn't support 10-bit H264
   decoding (confirmed by AW)" - jernej, LibreELEC Allwinner maintainer, A64/H3/H5/H6 thread, 2020-07-03.
2. **Hardware probes on this board, all with intact guards:**
   - M029: 8-bit 4:2:2 and 4:4:4 intra pictures (CAVLC and CABAC): CAVLC ends in an engine
     error, CABAC completes with wrong luma *and* chroma; the chroma-format bits 19..20 of
     PICSIZE do not repair either.
   - M030: I_PCM-only pictures, the case that removes transform, prediction and entropy
     decoding from the question: 4:2:0 exact, true 4:2:2 wrong for **all eight** values of the
     three-bit chroma selector (PICSIZE 21:19), 4:4:4 wrong. The PCM data path is fixed 4:2:0.
   - M034: valid 10- and 14-bit 4:2:0 I_PCM through the 8-bit path: fail (14-bit: error with no
     output writes).
3. **No register for it.** The engine's H.264 block (0x200..) has no bit-depth field in any
   observed programming (M014, all Linux traces M006/M024-M026), and the only output formats are
   8-bit NV12/tiled. By contrast the same engine family's H.265 block has explicit
   bit-depth/chroma fields in its SPS header register and, on the H6 only, a separate 10-bit
   output configuration (2-bit plane address and stride). Linux 6.12's cedrus (read for this
   capability question only; no code taken) refuses an H.264 SPS with `chroma_format_idc != 1`
   or any bit depth above 8 on **every** SoC variant, including the H6 that it lets decode
   10-bit H.265 - that is, the 10-bit capability exists in the family only for H.265, and only
   on a newer engine than the A64's (version 0x1689).
4. Vendor CedarX on FreeBSD (M036): none of three versions decoded any higher-depth/chroma stream.

Conclusion for the driver: 4:2:2, 4:4:4 and 10/14-bit H.264 are outside what the A64 video
engine decodes. There is no software fallback (owner's rule, M030). FMO and SP/SI remain as in
M033/M035/M037: no working register contract was found; they are refused the same way.

## F:M045-d - the driver now refuses those streams when the controls are set

Before: the refusal happened in `cedrus_h264_prepare`, i.e. only after the request was queued;
the application saw ERROR buffers (M029, nine injection checks). A player could not tell
"unsupported" from "damaged" before queueing.

Now: the media core has an optional per-control payload check (`struct vctrl_config.check`,
called in the validation pass of S/TRY_EXT_CTRLS after the size checks; nothing is set when it
refuses; the field is appended at the end of `struct vctrl`). The H.264 decoder registers
checks on three controls:

| Control | Refused (EINVAL) |
|---|---|
| SPS | `chroma_format_idc > 1` (mono and 4:2:0 stay accepted), any `bit_depth_*_minus8 != 0`, `SEPARATE_COLOUR_PLANE` |
| PPS | `num_slice_groups_minus1 != 0` (FMO) |
| SLICE_PARAMS (each element) | `slice_type` SP or SI, `colour_plane_id != 0` |

The same tests stay in `cedrus_h264_prepare` for a request whose controls were never set.

**Test** (`tests/cedrus_h264_reject.c`, runner `tools/h264-campaign/board-reject.py`, evidence in
[M045-reject/](M045-reject/result.json)): the stream is parsed without the engine's limits to
the first picture that carries the feature (for SP1_BT_A picture 10, the first SP slice), and
its six controls go to the driver as TRY_EXT_CTRLS, S_EXT_CTRLS on the current values and
S_EXT_CTRLS into a request. Real conformance streams: FREXT1_TANDBERG_A and Hi422FR1_SONY_A
(8-bit 4:2:2), Hi422FR13_SONY_A (10-bit 4:2:2), Hi422FREXT16_SONY_A (10-bit 4:2:2 interlaced),
FREH10-1 and PPH10I1_Panasonic_A (10-bit 4:2:0), PPH444I4_Mitsubishi_A and PPCV444I4_Mitsubishi_A
(14-bit 4:4:4, separate planes), SP1_BT_A (SP). Injected into f3 (no real FMO stream parses
past its PPS): fmo, sp, si, plane, plane_id, 422, 444, 10bit, 14bit.

Result: **18/18 refused** - every call returns EINVAL (TRY: error_idx at the offending control,
0 for SPS, 1 for PPS, 4 for slice params; SET: error_idx = count, nothing set), the current SPS
reads back unchanged, nothing is queued, and the driver counters `pictures errors rejects
timeouts` are identical before and after each case. The positive control (f3's own controls)
is accepted by TRY and by the request set; f3 then decodes **8/8** exact. Counters over the
whole assay: pictures 8 -> 16 (the f3 decode), errors 0, rejects 0, timeouts 0.

## Still open

Nothing in H.264 that the A64 engine can do is left undecoded in the conformance suite:
176/231 exact; the other 55 are 10/14-bit (40), 8-bit 4:2:2/4:4:4 (10), FMO (3), SP/SI (2),
all refused cleanly at control time. FMO and SP/SI are "no working programming found", not
"proven impossible" (M033, M035, M037).
