# HANDOFF — H.264 / Cedrus line (written 2026-10-07)

**2026-10-08 M043 — portability:** the driver no longer hard-codes DRAM_BASE/DMA_WINDOW. A per-SoC table
(`cedrus_socs[]` in `cedrus.c`, structure and "what differs on H3/H5/H6" in `cedrus_soc.h`) gives the engine's address 0,
its reach (2 GiB on A64; the old "256 MiB window" was never a limit), clock names and workaround flags; the CCU/syscon
addresses come from the device tree; RAM inside the reach comes from `vm_phys_segs` and attach fails loudly if there is none.
New read-outs `dram_base`, `dma_window`, `mem_lo`, `mem_hi`, knob `aux_lo_mb`, tunables `hw.cedrus.dram_base`,
`hw.cedrus.dma_window_mb`, `hw.cedrus.pll_gate`. Chimp guest unchanged and exact. See [M043](measurements/M043-portability.md);
host test `sys/dev/media/tools/cedrus-addr-test.c`. Not run on bare metal.

**2026-10-08 M041 - reserved buffer pool:** the engine's buffers (V4L2 OUTPUT/CAPTURE, H.264 MV/scratch) now come
from a pool taken once at attach (`hw.cedrus.pool_mb`, default 64, 0 = old behaviour; sysctls `dev.cedrus.0.pool_*`),
via a new optional `vbq_alloc_mem` method. On a guest where the old code failed 3/3 with ENOMEM at 1080p, the pool
decodes 3/3 and 3/3 without a reboot; 20 open/decode/close cycles leave `pool_used_kb` 0. The module refuses to unload
while pool buffers live. See [M041](measurements/M041-memory-pool.md); tools `pool-load.sh`, `pool-test.sh`, `tests/frag.c`.

**2026-10-08 M042 — a player:** `fbplay` (sys/dev/media/tools/ffmpeg/fbplay.c) plays mp4/mkv/ts/raw H.264 and MPEG-2
on the engine, zero copy to the KMS screen, audio (AAC/MP3/MP2/...) through OSS with the audio as master clock and
vsync-paced flips: 16200 frames of nine 60 s clips, 8 dropped, no A/V drift trend, H.264 frames identical to the
software decode. Trap: the board's `/dev/dsp0` plays at 0.9616 of the requested rate (pcm0 `failed to set sysclk`), so
use `fbplay -k 0.9616`. Build: `build-ffmpeg.sh configure make player push`; clips: `gen-clips.sh`. See
[M042](measurements/M042-player.md).

**2026-10-08 M039–M040 — FFmpeg and the screen:** FFmpeg's v4l2-request hwaccel decodes the 173 supported
JVT corpus streams (30 packets each) byte-identically through the driver; hardware decode runs at 82 fps at 1080p
with ~2 ms CPU per picture when the frames stay in dma-bufs, and `ffkms` shows them on the KMS screen without a copy
at 57-60 fps. See [M039](measurements/M039-ffmpeg-v4l2-request.md), [M040](measurements/M040-speed-and-kms.md);
tools in `sys/dev/media/tools/ffmpeg/` (`build-ffmpeg.sh` cross-builds on the host).

**Also read `HANDOVER.md`** (same directory): the working rules, the traps, the state of every tree and
the field/MBAFF/all-scenarios campaign with the exact point where it stopped.

Where this line stands, what was fixed, what is open, and how to get back into it.

**2026-10-08 M035–M037 — vendor route and a narrow FMO positive:**
44 measured executions,227 intact guard observations. Fifteen minimal SP tests
separate PCM/skip from explicit inter and I16x16: PCM exact, skip oracles disagree,
the latter two fail even without nonzero residual. An independent lab adapter
runs three opaque CedarX versions; all pass ordinary I/P/P and contiguous PCM,
but none exposes working general SP/SI, FMO, higher depth/chroma or mono PCM.
These adapter negatives are not a complete vendor-BSP or silicon-impossibility
proof. New FMO positive:396 single-PCM-MB slices in interleaved group order place
every Y/C sample exactly using known slice-start coordinates. General multi-MB
group traversal remains missing. Native recovery80/80 exact, errors/timeouts0.
Production scope remains176 streams, no software fallback. See
[M035](measurements/M035-minimal-switching.md),
[M036](measurements/M036-vendor-cedarx.md),
[M037](measurements/M037-fmo-single-macroblock.md). The research route now runs
directly on FreeBSD; repeatable tools, pinned vendor objects and full traces are
saved. Full hardware-only support remains open.

**2026-10-08 M032–M034 — remaining hardware hypotheses measured:**
46 lab executions,141 intact guard observations. SP PCM works exactly, but
non-PCM SP fails even with a checked JM encoder/decoder oracle; no type-field
value0..15 or tested QP/QS substitution fixes it. SI PCM fails at all16 type
values. Contiguous two-slice PCM is exact; interleaved FMO completes with wrong
pixels without a traversal map. Valid10/14-bit PCM fails in the known8-bit mode;
that is not a universal silicon impossibility proof. Native recovery80/80 full
Y/C exact, errors/timeouts/rejects0. No production support was broadened.
See [M032](measurements/M032-switching-boundary.md),
[M033](measurements/M033-fmo-traversal.md),
[M034](measurements/M034-depth-and-recovery.md). The full hardware-only request
remains open: a working register contract for the remaining modes is absent.

**2026-10-08 M031 — SP hardware trace obtained:** research-only SP parsing
(fbsd-media `f4450d4`) preserves all223 previously parsed control dumps and strict
refusal. Both official SP streams reach A64 through Linux: first10 luma hashes
match official YUV, first SP picture fails in hardware.3918 MMIO entries retained.
JM and FFmpeg disagree with the official SP pixels starting at that same picture;
no replacement oracle or production support is claimed. FreeBSD restored, native
f3 passes8/8 full Y/C, errors/timeouts/rejects0. [M031](measurements/M031-sp-linux.md).

**2026-10-08 M030 — hardware-only constraint and new boundary:** the owner
explicitly excludes software fallback. Ten-bit AVC is reported as unsupported
by the A64 platform maintainer (vendor confirmation cited);231/231 must not be
promised as a driver-only deliverable. True monochrome I_PCM is also now a
reproduced failure:420 I_PCM passes, mono fails through both lab and native driver.
All20 guarded PCM probes and recovery are in [M030](measurements/M030-hardware-ceiling.md).
M028's corpus result below remains valid, but does not mean every mono syntax works.

**2026-10-08 M028:** **176/176 supported streams, 19,028/19,028 full Y/C
frames exact**, zero engine errors/timeouts. Eight-bit monochrome now works,
including weighted prediction and the tested MBAFF combinations. Driver/front end:
`fbsd-media 3cd9cd7`, local, not pushed. [M028](measurements/M028-monochrome.md)
records the bit-19 A/B, engine-written neutral chroma, full regression and fixtures.

**2026-10-08 M027:** all **174/174 supported corpus streams** now decode exactly:
**18,828/18,828 full Y/C frames**. The measured LONG_TERM SRAM encoding fixes all
16 mismatches in MR8, MR9 and MMCO4. JM independently confirms their reference
lists. Source/capture DMA defaults now span 1GiB; two full corpus passes and probes
complete on one guest boot without allocation failures. See
[M027](measurements/M027-dma-longterm.md) for guards, controls, limits and raw evidence.
Implementation: `fbsd-media 8c87ce9`; oracle/replay tools: `11001f0`. Neither pushed.

[M026](measurements/M026-fields-mbaff-driver.md) established field/MBAFF support,
inherited SPS scaling matrices, direct8=0 MV geometry, EPB header skipping and
correct field pairing. Its three residual pixel failures are now resolved.
**55/231 streams remain outside the parser/driver scope:**27 ten-bit,
13 fourteen-bit,10 eight-bit422,3 FMO and2 SP. Generated444 and SI probes are
additional research inputs. [M029](measurements/M029-format-boundary.md) tests the higher-chroma first-I
candidate: 420 controls exact, 422/444 CAVLC errors and CABAC wrong pixels, all
guards intact. Simple PICSIZE extension is insufficient; a Linux refusal alone
is not proof of hardware inability. The missing contracts are listed in M029.

## Where it stands

- The native V4L2 core and the Cedrus decoder (MPEG-2 and H.264) live in **`freebsd-media`**
  — a sparse worktree, branch `media-v4l2`, **not pushed anywhere** (its `origin` is a
  fork). The H.264 decoder is committed (`26e72f5`).
- **H.264 decodes bit-exactly on the board** at 176x144 (`f3`, `wb`), 640x368 with P, B
  and B-pyramid (`t640`), 1280x720 (`hd720`), and on **all 33 committed streams**
  and every picture to the end (M023-g).
- Facts: **M001–M037** here. The H.264 ones: M014 (the programming), M015
  (`cabac_init_idc`, explicit B weights), M016 (the intermittent failure and what it is
  not — the long one), M017 (MV/scratch geometry), M018 (large pictures), M019 (buffer
  sizes measured), M020 (a mid-slice error loses the request), M021 (the stream set and
  the scratch reduction), M022 (the clock), **M023 (the cause of the intermittent error, fixed)**.
- The board is a Banana Pi M64 with the FreeBSD 15.1 guest; the test rig and the register
  lab are both restorable in one command (Tooling below).

## Fixed in this session

`freebsd-media` commits, newest first: the M023 fix and the test fix (see `git log`), `fca21a3` (a knob to ask the clock framework for a
different engine clock), `9f651ed` (the scratch formula divided by four, M021-b),
`103718c` (a knob to take the engine's buffers from the picture's segment), `c6cca28`
(the test stops feeding a picture whose slice failed, M020-g), `5191343`
(`board-bootstrap.sh`), `20d2ef1` (knobs to bisect the buffer sizes), `27f8652` (the
auxiliary allocation ladder reaches 1 GiB, so 1280x720 decodes, M018), `e1c568f` (the MV
halves back to Linux's `wmb*hmb*16`, M017-a — `a47da73` had it wrong), `88986b0` /
`e970183` / `d9c69ea` (instrumentation: frame hash through the direct map, reference FNV,
the buffers' memory attribute, the test's re-read check), `8489c1d` (`run-h264-test.sh`),
`26e72f5` (the H.264 decoder and its test).

## Open

1. ~~The residual ~6-7.5% engine error~~ — **found and fixed (M023)**: the wait after a
   header-skip command polled bit 16 of `0x228`, which is always set; the bit stream
   reader (busy = bit 8) was not waited for, and under DRAM load the next command raced
   its fetch. 0 errors in 2250 slices under load, against 12%. M016's and M022's
   "timing margin" and "V4L2 pattern" readings are superseded (M023-f).
1b. ~~Later pictures of eight streams~~ — a bug of the test, fixed (M023-g): all 33 streams
   decode to the last picture.
2. ~~The core strands a request after a failed held slice~~ — fixed in `vm2m` (M020-h).
3. ~~The scratch formula at 1080p~~ — `hd1080` decodes exact (M021-d).
4. **Remaining campaign work:** unsupported formats; long-term mismatches and
   repeated-corpus allocation acceptance are resolved in M027. The 231-stream JVT corpus contains the needed inputs;
   see HANDOVER §7.4 and M027–M029. All 231 entries are classified, but the 55
   remaining formats are not implemented. Do not treat Linux acceptance or matching Linux
   register programming as proof of correct decoded pixels.

## Tooling — how to get back in

- **`freebsd-media/sys/dev/media/tools/board-bootstrap.sh [streams...]`** — the first thing
  after any guest reboot: pushes the three modules (md5 verified), the shim and uapi
  headers, the test and the lab sources, and the streams with their ffmpeg references;
  builds `cedrus_h264` and `ve_lab` on the guest; loads `dmabuf_base` → `media` →
  `cedrus`. It exists because the guest's `/tmp` is a tmpfs and a reboot takes all of it.
- **The register lab:** `vectors/h264/README-lab.md` (feat/split/dump3 + `rp.py`), needs
  `kenv hw.cedrus.lab=1` before loading `cedrus` (and `hw.cedrus.lab_lo_mb` when the
  8 MiB buffer does not place). `H264_STRIDE` and `H264_STREAM_OFF` override what the lab
  programs.
- **Driver knobs** (all default to the normal behaviour): `mv_kb`, `scratch_kb`, `aux_near`,
  `aux_mb`, `src_mb`, `dma_mb`, `mod_mhz`, `h264_oldskip` (1 = the pre-M023 wait after a
  skip, for an A/B), plus the read-outs `h264_last` (addresses, `mixed`, the buffers'
  memattr), `skip_busy`, `h264_trace`, `rejects`, `errors`, `timeouts`, `missing_refs`, and
  `poke`/`poke_addr`/`peek` for the CCU. The M016-era experiment knobs (`h264_flush`,
  `h264_zero`, `h264_poll`, `h264_reset`, `h264_delay`, `h264_post_delay`, `h264_ack`,
  `h264_refsum`, `mv_min_kb`, `h264_dst`, `h264_ref`) were removed after M023: none of
  them had an effect.
- **The assay that found M023:** `errors` / `h264_last slices` after N runs of `smb` is a
  per-slice error rate; two `dd if=/dev/zero of=/dev/null bs=8m` on the guest load the DRAM
  and lift an error of 2.7% to 11-19%, which makes a 50-slice run decisive. Use it for any
  future timing question; a per-run PASS count at n = 50-100 was too weak (M016).
- **Analysis and A/B scripts:** `linux-work/h264-lab/h264/` (`tr.py`, `join.py`,
  `hlab.py`, `rp.py`, `cmp_trace.py`, `cmp_drv_linux.py`) and
  `linux-work/h264-traces/` (`run-cycle.sh`, `sweep.sh`, the A/B scripts, traces).

## Traps (each of these cost time)

- **The guest's `/tmp` is a tmpfs.** A reboot — a panic, the watchdog, or the engine reset
  the driver performs on a job timeout — takes the modules, the test binary and the
  streams with it. Use the bootstrap script; do not assume the modules are loaded
  (`kldstat | grep cedrus` first).
- **`kldload cedrus` without `media` fails with `No such file or directory`** — that is a
  missing *dependency*, not a missing file.
- **The lab buffer is 8 MiB contiguous and lands where it can** (0x4a000000 on this
  board). The regions near the 64 MiB boundaries are taken by the guest kernel, so a lab
  buffer that would span a segment boundary cannot be placed (M016-t).
- **`clk_set_freq` on the engine's clock is refused** (`error 34`). The divider is
  `0x01c2013c` bits [18:16] (M field, divide by `m`), the PLL is `0x01c20018`
  (24 MHz × N / M, both fields encoded as value−1, `mode_sel` bit 24, lock bit 28, enable
  bit 31). Both were restored to `0x03006207` and M=0 after M022; dividing the clock breaks
  the engine (2.5% → 12.5% → 42.5% → 100%), raising it to 336 MHz changes nothing.
- **The test's request poll is 5 s** and a lost request shows up as
  `request did not complete` — if that appears, the media core's request bookkeeping is the
  place to look, not the engine.

## State left behind

The H.264 changes and measured results belong to the named commits in this
line; neither repository was pushed. Unrelated dirty paths in bsdOS were left
alone. The final board/TFTP state and test provenance are recorded in M026/M027.

## Seen on 2026-10-07 and left alone

- MPEG-2 `play.m2v` (60 pictures) differs from ffmpeg by one or a few samples in 36
  pictures (`Y 101375/101376`), the same with the HEAD module before this session: not
  from H.264 work. Measured in [M038](measurements/M038-mpeg2-idct-rounding.md): IDCT rounding
  (no ffmpeg IDCT variant gives 60/60; they differ from each other far more than the engine does).
- Big pictures need big contiguous buffers: a guest that has run many tests refuses
  `REQBUFS` (1080p, 20 capture buffers) until it is rebooted. The test retries with fewer.
- The test compares the whole coded picture, so a reference yuv must be macroblock
  aligned (1920x1088, not 1080).
