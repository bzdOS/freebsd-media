# M035 — isolate the failing SP macroblock operations

2026-10-08. Fifteen generated176x144 CAVLC streams begin with a flat I_PCM
reference, then exercise one operation across99 macroblocks: PCM, skip,
explicit zero-MV inter prediction with CBP0, or I16x16 DC with zero DC token.
The three SP variants use QP26 with QS26, QS36, or QS36 plus
`sp_for_switch_flag=1`. Ordinary P controls cover the last three operations.
This separates the failures in M032 from nonzero residual complexity.
[Generator](M035-sp-minimal/fixtures.py), [parameters](M035-sp-minimal/fixtures.json).

Both external JM and FFmpeg decoders exit0 and return two complete frames for
all15 streams. Their full pixels agree for PCM, all P controls and SP I16x16.
For SP skip and explicit inter they disagree: JM's second-picture luma is127
at QS26 or130 at QS36; FFmpeg returns128. Both return neutral chroma128.
This disagreement is preserved rather than choosing the easier oracle.
[Validation](M035-sp-minimal/validation.json) includes hashes and exact commands.

## Observed Linux requests and guarded replay

One locked Linux build, one capture cycle,15 streams with two pictures each:
**5328 valid MMIO entries, zero invalid-syndrome entries**. The rig and kernel
make exit codes are0. Every leading I picture has the expected luma FNV
0xfa53b9c5. Linux completes SP PCM and skip, and reports hardware errors on
SP explicit inter and I16x16. FreeBSD SSH/UFS and its TFTP pairing are restored.
[Trace](M035-sp-minimal/linux/trace.txt),
[build](M035-sp-minimal/build-result.json),
[cycle](M035-sp-minimal/linux/result.json),
[summary](M035-sp-minimal/linux-summary.json).

The measured sequences are replayed serially through the bounded FreeBSD lab.
All15 leading I pictures are full Y/C exact. All **30 MV guard observations**
remain intact; no replay times out. A lab process exit0 alone is not a decoder
success criterion: error status and pixels are read separately.

| Second picture | Cases | Engine result | Full-pixel result |
|---|---:|---|---|
| SP PCM |3|done|exact against both oracles, including QS36 and switch1|
| P skip / explicit inter / I16x16 |3|done|exact against both oracles|
| SP skip |3|done|FFmpeg exact; differs from JM in all25344 luma bytes|
| SP explicit inter, CBP0 |3|error at MB0, raw0x5a|wrong/unchanged output|
| SP I16x16, zero DC token |3|error at MB0, raw0x5a|wrong/unchanged output|

[Full lab results](M035-sp-minimal/lab-results.json) and each case's
`normal/replay.lab`, `normal/lab.txt`, and `normal/result.json` retain the actual
commands, status/error/position and two independent comparisons.

SP PCM is an exact narrow positive. A done status on SP skip does not establish
switching correctness while its independent oracles disagree. The explicit
SP macroblock forms fail even with zero motion and no nonzero residual under
the measured programming. These experiments do not identify a working general
SP contract, and do not prove every undocumented contract impossible.
Production strict SP refusal remains unchanged.

Working artifacts: `linux-work/h264-traces/m035-sp-minimal/` and
`m035-sp-linux/`. Raw reconstructed frames and guards stay there; their hashes
are in [raw-sha256.json](M035-sp-minimal/raw-sha256.json). The source snapshots,
fixtures, traces and reports are committed; no software fallback is installed.
Final native recovery after M035–M037 is recorded in M037.
