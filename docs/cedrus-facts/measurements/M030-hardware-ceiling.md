# M030 — hardware-only scope, H.264 ten-bit limit and I_PCM boundary

2026-10-08. The owner explicitly requires **only the A64 hardware driver**, with
no userspace software-decoder fallback. Implementing a software decoder would
not satisfy that instruction. The current production module remains unchanged:
fbsd-media3cd9cd7, MD5 d57627e64bb4cfc21e64d264e9a19b5a.

## External capability evidence: ten-bit AVC

The LibreELEC Allwinner maintainer's [platform limitations](https://forum.libreelec.tv/thread/17565-nightly-images-for-a64-h3-h5-h6-and-r40-boards/)
identify ten-bit H.264 as a hardware limitation for that platform family,
including A64. In [post1122, 2020-07-03](https://forum.libreelec.tv/thread/17565-nightly-images-for-a64-h3-h5-h6-and-r40-boards/?pageNo=63),
jernej explicitly reports: “HW doesn't support 10-bit H264 decoding (confirmed by AW).”
That reply concerns an H6 report; the thread's platform-wide limitations provide
the A64 context. This is a maintainer's report of vendor confirmation, not a
new independent silicon measurement or a published vendor register specification.

This is stronger evidence than M029's absence from a product brief. It changes
the engineering scope: the27 ten-bit corpus entries cannot honestly be promised
as pending driver-only features. It does not by itself establish the exact limits
of every fourteen-bit, FMO, SP/SI or higher-chroma case. The full231/231 hardware
objective is not achieved; no software substitution was introduced.

## F:M030-a — discriminating I_PCM fixtures

Four locally constructed352x288 streams contain one intra picture made entirely
of I_PCM macroblocks: monochrome,420,422 and444, all eight-bit CAVLC. Every pixel
is prescribed by the generator. SPS/PPS, byte alignment, emulation prevention and
slice syntax are validated independently: ffmpeg decodes **all four** exactly
against those prescribed planes, without warnings. No lossy chroma conversion is
used for422/444. [Validation](M030-pcm/reference-verification.json),
[generator](M030-pcm/pcm-fixtures.py).

These inputs remove transform-coefficient and inter-prediction behavior from the
higher-chroma investigation. They do not remove the engine's macroblock parser.
The geometry, addresses,1MiB output reservation, separate MV arena and four4KiB
guards are the same as M029. The two scratch addresses are unchanged.

The community [VE register guide](https://linux-sunxi.org/VE_Register_guide)
describes a three-bit chroma selector at PICSIZE bits21:19. It is reverse-engineered
documentation across multiple SoCs, not an A64 conformance guarantee. The lab
therefore tests all eight values on mono and422 PCM, including diagnostic values
outside the standard's chroma_format_idc range. Production code still admits0/1.
Only that field changes within each input's series; error and position reads are
passive. No Linux/GPL implementation source is used.

| Input | Selector(s) | Result |
|---|---|---|
| True mono I_PCM | 0 through7 | all fail; no complete output |
| True420 I_PCM | 1 | exact152064-byte full NV12 |
| Same420 I_PCM bytes, deliberately mono controls | 0 | the same full colored NV12 is exact |
| True422 I_PCM | 0 through7 | all fail, Y and full-color output wrong |
| True444 I_PCM | 1 and3 | both fail |

All **20 executions preserve all four guards**. The only two successes use the
420 payload. The deliberately mismatched420/selector0 observation is not a valid
monochrome decode: it shows that this PCM path still consumes/outputs420 data
under the mono setting. For true mono/selector0, the raw position is0x100 and the
error register0x48; true420 completes at position0x1511 with error register0x40.
Those raw values are retained without assigning undocumented upper error bits.
[Results](M030-pcm/results.json), exact inputs/scripts/logs in the subdirectories.

This is evidence consistent with a fixed420 PCM data path, and establishes that
none of the eight selector values repairs the tested mono/422 inputs. It is not
a proof that no other hardware mechanism exists. In particular, blindly widening
output buffers or admitting more SPS values is not an implementation of the
missing input-syntax behavior.

A separate readback probe returns zero from PICSIZE for every value, including
the known-working420 setting and geometry. It therefore cannot establish which
bits were accepted; do not interpret zero readback as evidence that writes were
ignored. [Readback](M030-pcm/selector-readback.txt).

## F:M030-b — direct native reproduction and recovery

The ordinary, unchanged `cedrus_h264` test also fails the valid mono PCM stream:
one ERROR buffer, exit1. The valid420 PCM stream passes1/1 exact. Both uploaded
stream/reference pairs were MD5-verified. f3 then passes8/8 and weighted gray36/36
without reboot or reload: [native results](M030-pcm/native-results.json).

Final counters: pictures25260, rejects9, errors10, timeouts0; vbq has zero live
allocations and cedrus_dec one1KiB allocation. Nine errors are M029's deliberate
pre-engine rejections; the tenth is this deliberate reproduction of the mono
PCM engine error. [State](M030-pcm/final-state.txt).

**Correction to scope:** M028's two official mono streams and its weighted/MBAFF
fixtures remain valid exact results, but they do not establish arbitrary monochrome
H.264 syntax. True monochrome I_PCM is now a known failure, outside those corpus
fixtures. It must not disappear behind the176/176 corpus figure. A repair has not
been found; accepting the SPS alone does not establish complete profile support.

## Remaining work and feasibility

- 176 accepted corpus streams still have the M028 full exact regression.
- 27 ten-bit streams are outside the reported A64 hardware capability; they are
  not a promise of a forthcoming driver feature.
- The other28 refused entries (13 fourteen-bit,10 eight-bit422,3 FMO,2 SP/SI)
  remain unsupported. The corpus's ten eight-bit higher-chroma refusals are all422;
  M029/M030's444 probes are generated fixtures, not additional corpus entries.
- Monochrome I_PCM adds a known failure within the broader format accepted by SPS.
- No hardware implementation was added from failed probes, and no fallback was
  added. All231 hardware decoding remains unfulfilled under the owner's constraint.
