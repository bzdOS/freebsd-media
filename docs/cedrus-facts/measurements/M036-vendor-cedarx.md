# M036 — three opaque CedarX plugins on the actual A64

2026-10-08. An independent research host runs three unmodified AArch64
`libawh264.so` plugins from the official
[BPI-A64 Android7 repository](https://github.com/BPI-SINOVOIP/BPI-A64-Android7/tree/master/android/frameworks/av/media/libcedarc/library/lib64).
Variants `anou70`, `amm60`, and `alp50` each pass the ordinary three-picture
I/P/P control exactly. This opens a repeatable vendor-behaviour measurement
route without installing another OS or changing the native driver.

## Provenance and adapter boundary

[sdk-provenance.json](M036-cedarx/sdk-provenance.json) pins exact Git blob IDs,
SHA256 hashes and URLs for the three binaries and six generic public API
headers. Only public interface declarations, ELF metadata and opaque binaries
were used; decoder implementation source and algorithm disassembly were not
read or copied. The vendor binaries and separately fetched API headers are
**not redistributed in this BSD repository**.

The BSD-2 research adapter implements generic memory, stream-buffer,
frame-buffer and engine callbacks. A small ELF loader handles the AArch64
relocations and public imports, then calls the plugin's exported factory and
Init/SetSbm/Decode/Destroy interface. Its inaccessible MMIO page traps word
loads/stores and forwards them through the existing bounded `cedrus-lab`
ioctls. Unknown instructions or unrelated faults abort. DMA allocations stay
inside the8MiB lab arena and have4KiB guards; known VLD/DPB addresses are checked.
The observed version is0x16890040, reported to the plugin as0x1689.
[Adapter](M036-cedarx/cedar-probe.c), [loader](M036-cedarx/elf-loader.h).

This is an adapter experiment, **not validation of a complete stock vendor BSP**.
In particular, requested IRQ bits0..2 in register0x220 are masked before the
lab write to keep the native ISR from consuming completion; the wait callback
polls status, and reset uses the lab reset plus the known VE-top setting.
`mmio.txt` records the plugin's requested values, including the unmasked0x220
value; adapter callback reset operations are described by the adapter source.
These differences limit conclusions from negative tests.

The public smooth-frame-buffer configuration requests two extra buffers.
Output format7 is MB32 tiled Y and interleaved UV. The independent comparison
tool only untile/crops memory for full Y/C comparison; it does not decode
compressed pixels. [Comparison tool](M036-cedarx/compare.py).

## Completed matrix

All three plugin versions produce the same outcome for each of nine cases:
**27 runs,189 intact guard observations**. Every normal I/P/P control returns
3/3 exact frames; every contiguous two-slice PCM control returns1/1 exact frame.
The leading I pictures in SP and SI tests are also exact.

| Input | Slice decode jobs per version | Result |
|---|---:|---|
| Ordinary I/P/P, M032 checked JM input |3|3/3 full Y/C exact|
| Contiguous two-slice PCM, M033 |2|1/1 full Y/C exact|
| Checked encoded SP, QP26/QS36, M032 |2|leading I exact; SP error0x42, no SP picture|
| Interleaved multi-MB FMO, M033 |0|no picture; API returns NO_BITSTREAM5 before slice decoding|
| True monochrome PCM, M030 |0|API returns UNSUPPORTED−1 before slice decoding|
| Eight-bit422 PCM, M030 |0|API returns UNSUPPORTED−1 before slice decoding|
| Valid10-bit420 PCM, M034 |1|engine error0x42, no picture|
| Valid14-bit420 PCM, M034 |1|engine error0x42, no picture|
| I_PCM then SI_PCM, M032 |2|leading I exact; SI error0x42, no SI picture|

[Matrix](M036-cedarx/vendor-summary.json) links the27 case directories, each
with the requested MMIO trace, process log, upload MD5, execution metadata,
and full-pixel report where output exists. “Zero slice jobs” still permits
hardware bit-reader/header commands; it does not mean no VE accesses at all.
The FMO/mono/422 negatives are parser/API boundary measurements, not silicon
incapability proofs.

SP programs slice value0x1320 and QP register0x100001a without a distinct
measured QS36 switching contract. The10/14-bit inputs retain the observed
eight-bit PICSIZE/QP/slice settings0xd1511/0x100001a/0x1220. No successful
higher-depth programming contract was exposed. Some contiguous-control logs
show raw error0x5a after returning the exact picture; that later value does
not overturn the measured full-pixel positive.

Initial adapter diagnostics are retained separately: `control-1` faulted in
our buffer-size callback because optional stride outputs were NULL; it has no
final guard verdict. After that callback was fixed, `control-2` returned one
exact I picture and5 intact guards, then exhausted its single frame buffer.
The public extra-buffer setting produced the complete `control-3` baseline.
These diagnostic attempts are **not** included in the27-run/189-guard matrix.
One alp50-422 upload failed before any engine execution; the same case was
resumed with a successful MD5 check. The runner log is preserved.

## Reproduce and continue

Restore the owned tools into `linux-work/cedarx-research/`. `fetch-sdk.py`
fetches only the nine pinned public objects without a proxy and validates
their blob/SHA256 identities. `prepare.py` verifies those objects, MD5-checks
the guest source archive and builds the host against the native lab header.
Its final checked preparation exits0 with no compiler diagnostics.
[Preparation result](M036-cedarx/prepare.json).

After board bootstrap and console capture, enable the lab before loading
`cedrus`. Run `run-probe.py NEW-CASE ABSOLUTE-STREAM-PATH [ABSOLUTE-PLUGIN-PATH]`;
it locks the board, verifies every transfer and saves the trace. Use
`compare.py CASE-DIRECTORY PLANAR-REFERENCE` for emitted eight-bit420 pictures.
Do not overwrite completed cases or run alongside native decoding/another lab.
The harness is a research instrument; no proprietary plugin is added to the
production decoding path and no CPU reconstruction fallback is introduced.

Working artifacts, including opaque objects and raw pictures:
`linux-work/cedarx-research/`; their identities are retained in
[raw-sha256.json](M036-cedarx/raw-sha256.json). The native module is unchanged.
General SP/SI, FMO, chroma/depth and mono-PCM support remain open. M037 records
a separate narrow FMO positive and the final native recovery.
