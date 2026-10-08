# M028 — measured monochrome H.264 support

2026-10-08. Continues M027 with guarded first-picture experiments, then the native
FreeBSD driver (`fbsd-media 3cd9cd7`, local, not pushed). No Linux implementation source was read. The M026 Linux frontend
refused these SPS values; that refusal did not describe a hardware limit.

**Scope update (M030):** true monochrome I_PCM is a known failure. The exact
corpus/weighted/MBAFF results here remain valid for their tested inputs; they do
not certify all monochrome syntax. See [M030](M030-hardware-ceiling.md).

## F:M028-a — PICSIZE bit 19 and the output chroma plane

First-I-picture replay uses the existing measured 352x288 setup, with controls
from our front end and scaling matrices from each input. The paired scripts differ
in exactly one register write: H264_PICSIZE `0xd1511` versus `0x51511`, clearing
bit 19. All addresses, other controls, bitstream bytes and header-skip commands
are identical within each pair.

| Stream / entropy coding | Bit 19 set (4:2:0 setting) | Bit 19 clear |
|---|---|---|
| HPCVMOLQ_BRCM_B / CAVLC | engine error, status low bits 3 | exact full NV12 |
| HPCAMOLQ_BRCM_B / CABAC | completed but 114,477 wrong bytes | exact full NV12 |
| FRExt_MMCO4_Sony_B / colored positive control | exact full NV12 | not used for acceptance |

Every successful monochrome probe preserves both the output and MV canaries.
The output guard begins at 0x225200; MV guard at 0x22a000. All buffers are inside
the lab's 8MiB allocation. These observations establish bit 19 for monochrome
versus 4:2:0; they do not establish values for 4:2:2/4:4:4.
[Raw results](M028-mono/lab-results.json), with individual scripts/logs adjacent.

A separate CAVLC run pre-fills the UV plane with **0x5a** instead of neutral 0x80.
The engine still produces exact NV12 and every chroma byte becomes 0x80, with
intact guards: [result](M028-mono/HPCVMOLQ_BRCM_B-run-0-uv5a-result.json).
Thus neutral chroma is written by the engine; the driver needs no CPU fill or
postprocessing. Completion alone is insufficient: the CABAC negative control
completes normally while corrupting both Y and UV.

The native driver now admits chroma_format_idc 0 or 1 at eight bits and sets
bit 19 from that value. Capture remains NV12. Existing frame/field, MV and
long-term programming is unchanged. Higher chroma formats, higher bit depth,
separate color planes, slice groups and SP/SI remain explicitly refused.

## F:M028-b — monochrome weighted prediction syntax

For ChromaArrayType 0, pred_weight_table contains neither
chroma_log2_weight_denom nor chroma_weight_flag. The shared front end now skips
those fields, initializes neutral unused chroma weights, and retains the luma
weights and correct slice-header length. Its strict mode admits monochrome.

The generated 36-frame gray fixture has weighted_pred_flag=1 and
weighted_bipred_idc=2, with **31 actual weighted-reference entries** carrying
nondefault luma weights/offsets. Its parsed chroma_format_idc is zero; simply
asking an encoder for gray was not used as proof of monochrome syntax.
The native driver produces **36/36 exact Y/C frames**. A concatenation of those
36 gray frames, eight existing color f3 frames, and the 36 gray frames again
passes **80/80** through one decoder context, exercising both format switches.
[Controls](M028-mono/gray-weighted-controls.json),
[results and hashes](M028-mono/custom-driver-results.json).

Two additional gray MBAFF fixtures, CAVLC and CABAC, each parse as 36 MBAFF
pictures and pass **36/36 full Y/C frames** through the native driver:
[results](M028-mono/mbaff-driver-results.json). These exercise monochrome combined
with interlaced frame coding; they do not certify every PAFF/MBAFF combination.

Both complete official monochrome streams pass **100/100 frames each**, including
full neutral chroma: [native results](M028-mono/driver-mono-full.json).
The reference files match the official corpus YUV MD5 in both cases.

## Regression and scope

The new and pre-change front ends produce byte-identical canonical control dumps
for **all 174 previously accepted streams**:
[comparison](M028-mono/canon-420-regression.json). This compares the fields emitted
by canon.c, not every byte of every control structure. The complete strict-parser
inventory now accepts 176/231 and refuses 55/231:
[inventory](M028-mono/parser-scope.json).

The rebuilt module completes **176/176 streams, 19,028/19,028 full Y/C frames**,
with zero reference warnings and assay exit 0:
[full results](M028-mono/full-fixed.json), [all 231 classified](M028-mono/corpus-final.json).
165 references match the official corpus YUV files; eleven use ffmpeg uncropped
output, recorded individually. No allocation-induced omissions or guest reboot.
The final snapshot has **25,054 completed slice/field jobs, zero errors and zero
timeouts**, `vbq` zero live allocations, `cedrus_dec` one 1KiB allocation and
278,672 free pages: [snapshot](M028-mono/full-fixed-memory.txt).
Module MD5: `d57627e64bb4cfc21e64d264e9a19b5a`.

The 55 remaining parser refusals are 40 high-depth streams, ten eight-bit
4:2:2/4:4:4 streams, three FMO and two SP/SI:
[classification](M028-mono/unsupported-inventory.json). These are software scope
boundaries, not a claim of physical hardware impossibility.

Build uses matching earlyboot-wt headers through xbuild.sh cedrus, exit 0:
[log](M028-mono/build.txt). Verified upload hashes and guest test build output are
adjacent. Research-helper paths and fixture reproduction are described in
[M028-mono/README.md](M028-mono/README.md).
