# M032 — SP PCM works; switching reconstruction and SI remain unresolved

2026-10-08. Hardware-only investigation, unchanged production module
fbsd-media3cd9cd7 (MD5 d57627e64bb4cfc21e64d264e9a19b5a). No software fallback.

## Independent PCM fixtures

The generator creates352x288,8-bit420 pictures with prescribed Y/U/V samples.
Each stream starts with an IDR, then an I, P, SP or SI picture made entirely of
PCM macroblocks. SP uses QP=QS=26 and switch_flag0. The appropriate macroblock
type and, for P/SP, zero skip runs are encoded explicitly. Both JM19 and FFmpeg
decode all four complete two-picture streams exactly to the prescribed pixels.
[Generator](M032-switch/fixtures.py), [validation](M032-switch/validation.json).

The guarded lab feeds the second picture with no pixel reconstruction on the
CPU. It uses M030's measured register sequence,352x288 geometry,1MiB output
reservation and separate MV arena. Input lengths, header skips and slice type
are adjusted to the constructed syntax. No inter prediction is required by
these all-PCM pictures.

| Slice | Full152064-byte NV12 | Status |
|---|---|---|
| I | exact | done |
| P | exact | done |
| SP, type3 | exact | done |
| SI, type4 | wrong | engine error0x42, position0 |

**SP type alone is not universally rejected by the engine.** This positive
result does not exercise SP transform/prediction reconstruction and does not
justify admitting arbitrary SP requests in the native driver. The SI fixture
was also tried with every other value0..15 of the slice-type field: none gives
correct pixels or successful completion. No other register changes within that
series. All four memory guards remain intact in every PCM execution.

## New non-PCM SP oracle and Linux trace

JM19's reference encoder was built locally, used as an external executable,
without copying its algorithms into the BSD driver. Three QCIF clips contain
three pictures: ordinary P, SP with QP=QS=26, and SP with QP26/QS36. The input is
the reference distribution's `foreman_part_qcif.yuv`; exact encoder arguments
are retained in each `encode-command.json`.

For ordinary P, encoder reconstruction, JM decoder and FFmpeg all match. For
SP26, the three-way validation fails; that clip is diagnostic only. For **SP36,
JM decoder exactly matches the encoder reconstruction**, while FFmpeg differs.
SP36 therefore supplies an independently cross-checked reconstruction oracle
for this particular switching case, without resolving the older official JVT
discrepancy. [Validation](M032-switch/encoded-validation.json).

One locked Linux Image build, H264_MAX_PICS=3, embeds all three clips. The
versioned cycle captures **1246 MMIO entries, isv0=0**, then restores FreeBSD
TFTP pairing and SSH/UFS. Ordinary P completes all3 pictures; both SP clips
complete the first I picture and return ERROR on their first SP picture.
[Trace](M032-switch/linux-trace.txt), [console](M032-switch/linux-console.txt),
[recovery](M032-switch/linux-result.json). No Linux/GPL implementation source
was read; this is black-box register observation.

## Relocated replay and bounded alternatives

The measured requests were replayed in the guarded8MiB lab, retaining references
to the preceding I picture. The ordinary P control is **3/3 full NV12 exact**
against JM encoder reconstruction. Every other replay's leading I picture is
also exact. SP36 fails at macroblock0 with error0x42; SP26 fails with raw0x5a.

For SP36, every value0..15 in the four-bit slice-type field was tested. None
matches the checked JM reconstruction. Values0 and5 can finish without an
error, but produce incorrect pixels. Replacing the QP register value26 by QS36
was separately tested for types3,0 and5; none repairs reconstruction. The SP26
as-P diagnostic also finishes with wrong pixels. These tests eliminate a simple
type-code substitution and the tested single-QP substitution, not every possible
unpublished switching-mode register contract.

The [community register guide](https://linux-sunxi.org/VE_Register_guide) is the
source for the bounded slice-type field, not evidence of A64 SP/SI conformance.
No trigger-command or address-register sweep was performed.

Across **41 lab executions, all121 guard observations are intact**.
[Aggregate results](M032-switch/results.json), per-run scripts, register reads
and upload hashes are retained alongside them. Large raw dumps remain under
`linux-work/h264-traces/m032-switch`; [SHA256 inventory](M032-switch/raw-sha256.json).
Each generator is the exact fixture source; replay scripts retain the exact
commands issued to the lab. The oracle is used only for testing, not deployment.

No production SP/SI support is added. A working switching reconstruction
contract remains missing. Native recovery after the entire M032–M034 series is
80/80 full Y/C exact with errors/timeouts/rejects0; see M034.
