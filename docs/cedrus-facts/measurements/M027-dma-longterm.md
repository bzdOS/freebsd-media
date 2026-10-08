# M027 — DMA window and independent long-term reference checks

2026-10-08. Continuation of M026. Linux implementation sources were not read.
The long-term fix and repeated DMA-window regression are complete. Additional
format support is a separate investigation, not implied by this result.
[M028](M028-monochrome.md) subsequently establishes eight-bit monochrome support.
Implementation: `fbsd-media 8c87ce9`; comparison/replay tools: `11001f0`.
Both commits are local, not pushed.

## F:M027-a — data-buffer address range

With the M026 module unchanged, setting both `dev.cedrus.0.dma_mb` and
`dev.cedrus.0.src_mb` to 1024 permits data buffers outside the old 256MiB window.
The initial five-stream probe completes exactly: HCHP3_HHI_A (250 frames),
HVLCFI0_Sony_B, HVLCMFF0_Sony_B and HVLCPFF0_Sony_B (19 each), and
alphaconformanceG (40): [probe](M027-lab/dma-probe.json).

The following uninterrupted complete-corpus run reproduces **171/174 accepted
streams exact, 18,812/18,828 frames**, with the same three pixel mismatches and
no allocation failure: [results](M027-lab/dma-full.json). The initial catalog
filter selected 180 streams; six extra entries still fail for FMO, SP/SI or
monochrome syntax, as in M026. The assay correctly returns exit 1 rather than
claiming an all-pass run.

Afterward the driver's `pictures` counter is 24,991 on this boot (completed jobs,
including individual H.264 slices/fields, not whole output frames), with zero timeouts,
and still just the one intentional truncated-input error from M026. `vbq` is
back to zero live allocations, `cedrus_dec` to one 1KiB allocation; 302,357 pages
are free: [snapshot](M027-lab/dma-full-memory.txt). Last-slice relative addresses
are `src=0x3ff40000`, `luma=0x3f840000`, near the top of the 1GiB window.
[Earlier addresses](M027-lab/dma-addresses.txt) also exceed 256MiB.
The rebuilt default-window regression is recorded below.

A subsequent same-boot comparison of five allocation-sensitive streams gives
**5/5 at both 256 and 1024MiB** ([256](M027-lab/window-256.json),
[1024](M027-lab/window-1024.json)). Thus the original narrow-window failure is
history-dependent and was not reproduced by this short A/B. The wider default
removes an unnecessary placement restriction; these data do not prove that
fragmentation can never cause an allocation failure.

The existing auxiliary allocator already grows to at least 1GiB (M018). The
decoder data-buffer defaults can use that measured range without changing the
lab's allocation preference or the hardware address representation.

## F:M027-b — independently decoded official reference

JM 19.0 decodes all three remaining failing streams exactly against the official
JVT YUV files, including after adding a printf-only list observer:
[pixel results](M027-lab/jm-list-results.json).

| Stream | Output bytes | Official/JM MD5 |
|---|---:|---|
| MR8_BT_B | 2138112 | `1ed4829bf27262147b12ee16cd6ce5db` |
| MR9_BT_B | 2138112 | `c1ae4e809894d01cc4f5e6b0bb8df99e` |
| FRExt_MMCO4_Sony_B | 9123840 | `b10b53b906890855c0d8c36b58791957` |

Provenance: the `linzhenan/JM` mirror at
`fab245b35f55bdea1ccace6cb334642015b7fffc`, downloaded as a ZIP from GitHub.
The reference package includes `COPYRIGHT_ITU.txt` and `COPYRIGHT_ISO_IEC.txt`;
it is an ISO/ITU reference decoder, not Linux/GPL driver implementation code.
No decoder algorithm was transferred into the driver. The scratch checkout is
`linux-work/h264-traces/m027-oracle/JM-master`.

Build on the current host: `LC_ALL=C make -C ldecod -j8 CFLAGS=-fcommon`.
Run `bin/ldecod.exe -d bin/decoder.cfg -p InputFile=... -p OutputFile=...
-p RefFile=...` with absolute paths and a separate output working directory.
The [observer patch](M027-lab/jm-observer.patch) only prints list entries after
reordering. An earlier `PRINTREFLIST=1` diagnostic also remained enabled;
the comparison uses only the custom `ORACLE` lines.

## F:M027-c — list identities agree on every slice

`jm-list-compare.py` compares independently observed JM lists against `canon.c`
output from the same tracked `h264ctl.h` used by the hardware rig:
[comparison](M027-lab/reference-lists-comparison.json).

| Stream | Slices | Current-picture / reference-identity differences |
|---|---:|---:|
| MR8_BT_B | 116 | 0 |
| MR9_BT_B | 64 | 0 |
| FRExt_MMCO4_Sony_B | 60 | 0 |

Identity includes ordered POC, frame/field structure, long-term flag and long-term
index. The current picture's frame_num, POC and structure also match. Twelve MR9
raw comparisons differ only in reference frame_num: JM's split-field entries
report zero. Those differences remain visible in the JSON; the identity comparison
does not silently call the entire raw control stream identical. These clips have
one POC epoch; the comparator is not a general identity mapper across POC reuse.

The adjacent `jm-*.txt` and `canon-*.txt` preserve both observations. A deliberately
changed current-picture frame_num produces one mismatch and exit 1:
[negative control](M027-lab/jm-negative.json). The real three-stream comparison
returns exit 0. This narrows the remaining investigation to controls outside the
compared list identities and their handling by the driver/engine; it does not
establish a hardware encoding for long-term references.

JCodec 0.2.5 was also tried as a secondary observer, but matched only 17/60 MMCO4
frames. Its reference decisions were therefore not used as correctness evidence.

## F:M027-d — long-term metadata: measured working programming

Relocated replay reproduces all four early failing frames, with intact MV guards:
MMCO4 **23/24**, MR8 **27/28**, MR9 **30/32** exact full NV12 frames. Inputs,
geometry, lists and all register writes come from the saved M026 Linux traces.

The controlled probe changes only SRAM picture-table **word 2** of entries
classified LONG_TERM by the front end: OR **0x11**, preserving the independent
0x100 field and 0x200 MBAFF modes. This makes **24/24, 28/28 and 32/32 exact**, with
every MV canary intact. No address or engine-control register is modified.

- [MMCO4 probes](M027-lab/m027-lt-lab-results.json): masks 0x01 and 0x02 also
  fix this progressive clip; 0x04, 0x08, 0x10, 0x20, 0x40 and 0x80 do not.
- [MR8 probes](M027-lab/m027-lt-mr8-results.json): 0x01 alone leaves 1,105 wrong
  bytes in the failing frame, 0x10 alone leaves 5,030; their union 0x11 leaves
  zero. Baseline has 6,099 wrong bytes. 0x22/0x33 also happen to pass this clip;
  that does not establish them as equivalent encodings generally.
- [MR9 independent check](M027-lab/m027-lt-mr9-results.json): 0x11 eliminates
  both earlier failures (18,798 and 13,784 differing bytes).

MMCO4's additional **last-picture-only** experiment alters just two word-2 writes
in decoded picture 23, leaving all earlier pictures exactly at baseline. It still
produces 24/24 exact frames. Thus its fix does not require changing earlier motion
vector contents: [script](M027-lab/mmco-last-only.lab), result in MMCO4 probes.

The driver uses the measured 0x11 operation only for DPB LONG_TERM entries. These
observations establish a working entry encoding, not the meaning of every bit
or all reserved values. All three **complete streams** now match on the rebuilt
driver: **MR8 58/58, MR9 58/58, MMCO4 60/60**, eliminating all 16 mismatching
frames from M026: [results](M027-lab/driver-longterm.json). Every provided-reference
MD5 matches the official corpus YUV. The complete regression is recorded below.

The module was reloaded without rebooting the guest. The new default knobs both
read 1024: [reload](M027-lab/driver-reload.txt),
[verified uploads](M027-lab/driver-upload-md5.json). Existing H.264 f3 is 8/8 exact;
MPEG-2 flat, grad and i64 also compare in full Y/C. Their logs are adjacent.

Reproduction uses the versioned `replay.py --canon ... --long-term-mask 0x11`.
Generate the exact clip's manifest/slices with `export.c` and its DPB classification
with `canon.c`. In `longterm1/trace.txt`, MR8's 56 slices start at run 0, MMCO4's
24 at run 56. In `weighted2/partial-trace.txt`, MR9's 35 slices start at run 24.
Use MV factor 2 for MR8/MR9 and 1 for MMCO4. The archived scripts and lab logs
record the precise executed writes. The versioned generator reproduces the
successful MMCO4 script byte-for-byte.

## F:M027-e — complete driver regression and sustained allocation

The rebuilt module completes **174/174 accepted streams, 18,828/18,828 whole
frames exact in Y/C**, with no reference-decoder warnings:
[full result](M027-lab/full-fixed.json), [all 231 classified](M027-lab/corpus-final.json).
163 complete reference files also match the official corpus YUV MD5; the other
eleven comparisons use ffmpeg's uncropped output, as individually recorded.
The three former failures match both ffmpeg and the official files. The supported
corpus assay returns exit 0, without retries or allocation-induced omissions.

This is the second full corpus pass on the same guest boot. Before the module
reload, its successful-job counter was 25,897; afterward it reaches 24,781:
**50,678 completed jobs** total, including probes, without a guest reboot.
The rebuilt module ends with **zero engine errors and zero timeouts**, `vbq`
at zero live allocations, `cedrus_dec` at one 1KiB allocation, and 280,027 free
pages: [snapshot](M027-lab/full-fixed-memory.txt).
These observations meet the repeated-corpus acceptance criterion. They do not
promise success for arbitrary memory pressure or prove the historical
fragmentation hypothesis; the short 256MiB A/B above passed too.

The driver changes are the measured long-term word-2 encoding and 1024MiB
defaults for source/capture placement. The lab's old allocation window is unchanged.
Module MD5: `c419fda59092bec9f1a9110c421b0663`.
Build: matching `freebsd-src-earlyboot-wt` headers via `xbuild.sh cedrus`, exit 0;
[build log](M027-lab/build.txt), [guest identity](M027-lab/full-fixed-identity.txt).
FreeBSD remains running, with its original TFTP pairing untouched in this session.

## Remaining format boundary

A Linux SPS rejection for monochrome, 4:2:2 or 10-bit (M026) still does not prove
an A64 hardware limit or supply programming for those formats. The remaining
57 refusals are [classified](M027-lab/unsupported-inventory.json): 40 high-depth
(27 at 10 bits, 13 at 14 bits), ten 8-bit 4:2:2/4:4:4, two monochrome, three FMO,
and two SP/SI. High depth takes precedence over chroma in these disjoint counts.
Guarded monochrome first-I probes and a weighted monochrome fixture are prepared
in `linux-work/h264-traces/m027-mono`; no monochrome capability is claimed here.
