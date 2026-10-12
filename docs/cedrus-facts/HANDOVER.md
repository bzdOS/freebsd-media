# HANDOVER — native V4L2 + Cedrus (written 2026-10-07 for the next agent)

Everything the previous agent knew about this line that is not already in the numbered facts
(M001–M037) or in `HANDOFF.md`: how to work here, what state everything is in, the traps, and the
campaign that was in flight when the work stopped. Read `HANDOFF.md` first for the decoder itself,
then this file.

## 1. The project in one paragraph

A clean-room, BSD-2 V4L2 stack (core, dmabuf, media controller, request API, m2m scheduler) for
FreeBSD base, plus a driver for the Allwinner A64 video engine ("Cedrus") that decodes MPEG-2 and
H.264 through the stateless V4L2 interface, with zero copy to the GPU (Mali-400 via lima) and KMS.
Target: Banana Pi M64 running FreeBSD 15.1 as a guest of the "chimp" hypervisor, which owns the
display. The Linux/GPL sources (v4l2-core, videobuf2, cedrus, hantro) are **not** to be read or
ported for the driver: the register programming comes from hardware observation (the hypervisor logs
every VE register access of an unmodified Linux 6.12 guest, `HV_VETRAP=2`). Reading Linux *behaviour*
(traces) is the method; reading Linux driver *source* is not. (Linux's UAPI headers are fine; the
`uapi/linux/*.h` in `sys/dev/media/uapi` are the published interface.)

**Owner clarification2026-10-08:** only the A64 hardware driver; no userspace
software-decoder fallback. M030 records the reported ten-bit hardware limit and
new valid mono I_PCM failure. Do not equate176 exact corpus streams with full
profile conformance, or promise all231 solely by changing the driver.

**Latest: M035–M037.** The vendor black-box route is operational on FreeBSD:
owned generic ELF/API/MMIO adapter, three pinned opaque CedarX versions,
27 measured runs/189 intact guards; normal I/P/P and contiguous PCM controls
exact on every version. No general remaining format works through that adapter;
FMO/mono/422 stop before slice decode, so those negatives do not establish a
silicon limit. Fifteen minimal SP probes add30 intact guards: PCM exact, SP skip
oracles disagree, explicit inter and I16x16 fail even with no nonzero residual.
New narrow FMO positive:396 one-PCM-MB slices in interleaved group order decode
full Y/C exactly, as does raster control (eight guards intact). It uses known
slice-start coordinates; it does not solve inside-slice traversal or enable the
three official FMO streams. See M035–M037 and their committed evidence/tools.
Total44 canonical runs/227 intact guard observations; native recovery80/80 full
Y/C exact, fresh errors/timeouts/rejects/missing_refs0, vbq live0. Driver/module
unchanged; full hardware-only task fedora-183 remains open. The remaining55
official streams are27 ten-bit,13 fourteen-bit,10 eight-bit422,3 FMO and2 SP.
Console follower PID3249662 was stopped at the owner's request; its capture is
`linux-work/h264-traces/m035-sp-minimal/console-after-linux.txt`. Start a fresh
follower before the next native module test. Running HV build5137e4fca675 differs from
local ELF d1c6f75d9105: do not use local symbol addresses for HV function calls.
FreeBSD TFTP matches the backup; the lab remains enabled with its8MiB arena.

**M032–M034 continuation:** all enumerated switching/FMO/depth probes are
finished:46 lab executions,141 intact guard observations. SP PCM is exact;
non-PCM SP is not repaired by any4-bit slice-type value or tested QP/QS mapping.
SI PCM fails all16 values. FMO ignoring traversal returns wrong pixels despite
done status; contiguous control is exact. Valid10/14-bit PCM fails in known8-bit
mode. These are bounded negatives, not a proof about all unpublished modes.
See measurements M032–M034 for exact fixtures, independent validation and traces.
Native recovery80/80 full Y/C exact, fresh errors/timeouts/rejects0. Production
module unchanged; no software fallback. For further feature implementation a
new measured or documented switching/traversal/depth/chroma contract is needed;
repeating the exhausted selector sweeps is not a new hypothesis. fedora-183 stays
open, since general support is not achieved. The console state above supersedes
the historical M032 follower PID3135592.

## 2. Where things are

| What | Where | Notes |
|---|---|---|
| Code, tests, tools, docs (everything) | `/opt/bzdos/freebsd-media` = `github.com/bzdOS/freebsd-media` (public since 2026-10-12, branch `master`) | the only working tree since 2026-10-08. `/opt/bzdos/fbsd-media` is a symlink to it so old scripts keep working; the old FreeBSD-fork worktree is gone (branch `media-v4l2` still exists in `fbsd-upstream`, frozen). Old commit hashes in the facts map to new ones in `docs/cedrus-facts/COMMIT-MAP.md` |
| Facts, docs | `/opt/bzdos/docs/cedrus-facts/` | `HANDOFF.md`, `HANDOVER.md` (this), `measurements/M001..M023`, `vectors/` |
| Spec | `bsdOS/docs/specs/SPEC_media_v4l2_native.md` | |
| Kernel headers for the board ABI | `/opt/bzdos/freebsd-src-earlyboot-wt` | build modules only against these (`tools/xbuild.sh`) |
| Linux rig, traces, corpus | `/opt/bzdos/linux-work/` | not in git. `h264-traces/` (cycle scripts + traces), `h264-lab/` (analysis, replay), `initramfs-h264/` (rig `/init`), `jvt/` (conformance corpus), `h264front/` (scratch of the front-end work), `build-min/` (Linux build tree) |
| Hypervisor + board tools | `/opt/bzdos/microkernel` | `bzdctl.py`, `vetrace.py`, `vcon_dump.py`, `safemode.py`, `hvdbg.py`; read `SESSION-RULES.md`, `DEBUG_RULES.md` |
| Guest | `ssh -i /root/.ssh/chimp_ed25519 root@192.0.2.82` | root is read-only, `/tmp` is tmpfs and **wiped by every reboot**, modules load only from `/tmp` |


## 3. Standing rules (the owner's, they override defaults)

- Commits: author **and** committer `Andrey Bodrov <ap.bodrov@gmail.com>`, no `Co-Authored-By`, no
  "generated with" trailer, in every repo. Set `GIT_AUTHOR_NAME/EMAIL` and `GIT_COMMITTER_NAME/EMAIL`
  explicitly. Commit named files only; never `git add -A` / `git add .`.
- Never push without the owner's word. Never `git stash` bare (shared stash stack).
- Never `find`/`grep` from `/` or run host-wide scans; search inside named directories.
- Local/LAN targets: `curl --noproxy '*'`; the shell exports a proxy that cannot reach the LAN.
  For *downloads* from the internet unset the proxy variables (`env -u http_proxy -u https_proxy`).
- Never ask the owner to reset or power-cycle the board. A network reload is yours to do:
  `ssh ... 'shutdown -r now'` (clean hypervisor reload) or the `wdt_reset` in `hvdbg.py`.
  After five quick boots of one image the board enters safe mode: `python3 safemode.py force 0`.
- Never write an untested bootloader/boot config to media; never leave Linux files in
  `/opt/bzdos/tftpboot` (`run-cycle.sh` restores them from `.fbsd-backup` on exit — check after).
- Build output is Russian: use `LC_ALL=C` and check the make exit code.
- `make sema-check` before every commit in `bsdOS`.
- `cp` is aliased interactive on this host: use `command cp -f`. An interactive prompt in a tool call
  hangs it.
- Do not end a report with "shall I start?"; do the obvious next step. Ask only at real forks.
- Be honest: report failures with the output; retract a wrong claim and say so. In this line the
  worst time losses were all wrong *measurement criteria* (below), not hard bugs.
- Use they/them for people whose pronouns are not stated.

## 4. Board and guest operations

- **Bring the rig up after any guest reboot:** `freebsd-media/sys/dev/media/tools/board-bootstrap.sh
  [streams...]` (pushes dmabuf_base/media/cedrus with md5 check, shim + uapi headers, the test and the
  lab sources, streams and references; builds `cedrus_h264` and `ve_lab` on the guest; loads the
  modules in the order `dmabuf_base` → `media` → `cedrus`). `kldload cedrus` without `media` says "No
  such file or directory": that is a missing dependency.
- **Panics leave no trace.** Before testing a module start the console follower:
  `cd /opt/bzdos/microkernel && python3 bzdctl.py console --follow > $FILE &`, and kill it later by
  PID (`pgrep -f "bzdctl[.]py consol"`, in a *separate* command).
- **Never use `pkill -f` / `pkill` with a pattern that occurs in your own command line** (even inside
  a heredoc): it kills your own shell (exit 144). This happened three times in this line.
- **Never run two kernel builds at once** in `linux-work/build-min` (a second `make` deletes
  `include/config/.tmp_kernel.release` under the first and both fail). Run one `make ... Image`, wait
  for it. Foreground `sleep` is blocked by the harness: wait with an `until` loop in a background
  command or the Monitor tool.
- Modules pushed to the guest: verify with `md5` against the host file (a crash after `scp` leaves
  the right size and bad content). After a Linux cycle or a panic the guest's `/tmp` is empty: run the
  bootstrap again. The guest has ~850 MB usable RAM; contiguous allocations fragment after many runs
  (`REQBUFS ... ENOMEM` on big pictures): reboot the guest (`shutdown -r now`, ~70 s) and bootstrap.
- Guest-side scripts (in `tools/`): `all.sh <runs> idle|hog` runs every stream on the guest to the last
  picture and counts runs (a run is good only if every picture line is PASS and none of FAIL / ioctl
  errors / "did not complete" appears); `assay.sh label "sysctl k=v ..." runs` and `rate.sh runs stream`
  give the per-slice engine error rate from the driver's counters (see M023).
- `kenv hw.cedrus.lab=1` (+`hw.cedrus.lab_size`, `hw.cedrus.lab_lo_mb`) before loading `cedrus` enables
  `/dev/cedrus-lab`; it takes 8 MiB of contiguous memory; reboot if it cannot place it. Set `kenv` on the
  guest itself (a shell variable in your command line is expanded on the host).
- To unload a module loaded from a differently named file use `kldunload cedrus` only if the file is
  `cedrus.ko`; for `cedrus-head.ko` use `kldunload cedrus-head.ko`.
- The sysctls: `dev.cedrus.0.{errors,timeouts,rejects,missing_refs,pictures,skip_busy,h264_last,
  h264_trace,h264_oldskip,mv_kb,scratch_kb,aux_near,aux_mb,src_mb,dma_mb,mod_mhz,poke,poke_addr,peek}`.

## 5. Hard-won lessons (each cost hours)

1. **A completion bit that is constant in the reference trace is not a completion flag** (M023: bit 16
   of the H.264 status was always set; the real flag is bit 8 = reader busy). When a register wait
   "works" in the lab but races in the driver, look at what the wait actually tests.
2. **A lab that is clean 100/100 may differ only in write spacing** (a syscall per register write).
3. **Measure error rates per event, with a positive control.** Per-run pass/fail at n = 50–100 hid
   everything for days; errors per slice with a DRAM hog (`dd if=/dev/zero of=/dev/null bs=8m` ×2)
   as a positive control gave a decisive answer in minutes.
4. **"No failure text" is not success.** Count PASS lines and detect ioctl/ENOMEM; a run that cannot
   start is not a pass. Two earlier "findings" (address windows, low-memory workaround) came from
   counting such runs as good and were retracted.
5. **Check the test before the driver.** The "late pictures of eight streams" bug was the test decoding
   a picture into the buffer of its own reference (the DPB it read was already past the picture).
6. Memory attributes: on arm64 FreeBSD the device pager takes a page's memattr from the `vm_page`,
   not from `d_mmap`; engine buffers must be uncached (M009). Hypervisor reads of guest DRAM are
   cacheable and cannot prove what a non-coherent DMA engine saw.
7. `pll_ve` lacks `AW_CLK_HAS_GATE` in `ccu_a64.c`: `clk_enable` does not switch it on and the first
   VE register read hangs the bus (patch in `hal/lima/patches`). The driver gates it itself.
8. `clk_set_freq` on the engine clock is refused (error 34); the divider is `0x01c2013c` bits 18:16,
   the PLL `0x01c20018`. Dividing the clock breaks the engine; raising it changes nothing (M022).
9. FreeBSD `kern_ioctl` does not copy data out on error: V4L2's `error_idx` needs the core to copy out
   itself. Only two `selrecord()` per thread per scan: one `selinfo` per vdev. v4l-utils on FreeBSD
   knows only capture devices: `tools/v4l-m2m-patch.py` patches the tools (`compliance.sh` does it).
10. After a guest reboot the stock `dmabuf.ko` is loaded; for zero copy to the GPU swap in the bridged
    one from `/opt/bzdos/fbsd-obj/drm-kmod-arm64/dmabuf/dmabuf.ko` (unload bzfb, lima, drm first).
11. The MPEG-2 sample `play.m2v` is ±1 sample off ffmpeg in 36 of 60 pictures with every build; IDCT
    rounding is the suspect, never investigated.
12. The test compares whole coded pictures: a reference yuv must be macroblock aligned (1920x1088,
    decode with `-apply_cropping 0` for streams that crop).

## 6. Historical baseline (2026-10-07; current work is in §7.4 and M026)

`freebsd-media` (branch `master`, pushed to the private GitHub repo on 2026-10-08; hashes quoted
below and in the facts are the OLD ones, see `COMMIT-MAP.md`): H.264 decoder and test, the M023 fix,
the vm2m orphan fix, `tests/h264ctl.h` rewritten (see 7), `tools/h264-campaign/`, guest scripts.
Last commits (newest first): the front-end rewrite, `-k` / retry in the test, vm2m orphan fix, knob
removal, test fix (do not decode over a reference), M023 fix.
The facts, `HANDOFF.md` and this file live in `docs/cedrus-facts` of this repository; `bsdOS` keeps only a pointer.
Guest: modules (`cedrus.ko` final build) loaded from `/tmp`, test binaries and 33+ streams in
`/tmp/ve`; the lab is disabled. `/opt/bzdos/tftpboot` is in its FreeBSD state (verify with the
`.fbsd-backup` md5s before and after any Linux cycle).

What the decoder does today: MPEG-2 I/P/B frames (and interlaced frame pictures) within ±2 per sample of ffmpeg (M038-a: the IDCT is not bit-exact); H.264 frame
pictures, 4:2:0 8-bit, all 33 test streams exact to the last picture (up to 1920x1088, I/P/B, 3–8
refs, CABAC/CAVLC, 8x8, scaling lists, explicit P weights with denominators ≤ 6 and offsets, 15
slices a picture), 0 engine errors in 2250 slices under DRAM load. That was the M023 baseline. M026 adds field/MBAFF decoding and fixes direct8=0
MV geometry. M028 adds eight-bit monochrome, including weighted syntax;
4:2:2/4:4:4, higher depth, slice groups and SP/SI remain refused.

## 7. The campaign that was in flight: "all scenarios" (field, MBAFF, the rest)

The owner asked: capture the Linux register traces of **all** scenarios, then implement. This is the
plan and measured progress. Linux builds/capture, field-aware replay and field/MBAFF implementation
are now complete; see "Resume here" for the remaining specific failures.

### 7.1 The corpus (done)
The official JVT conformance suite was downloaded: `linux-work/jvt/{AVCv1,FRExt,Professional_profiles}`
(231 zips, 1.6 GB; source `https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/`, reachable
with the proxy variables unset). `jvt/catalog.py` unpacks to `jvt/streams/<name>/` and writes
`jvt/catalog.json` (profile, size, SPS/PPS/slice flags, ffmpeg md5, provided-yuv md5). Facts:
- 173 of 231 have a provided yuv equal to ffmpeg's decode; the rest are streams without a yuv (MW
  baseline set), 10-bit/4:2:2/4:4:4 (professional profiles; ffmpeg pix_fmt differs), FMO/ASO, SP/SI.
- Profiles: 90 Main, 33 Constrained Baseline, 43 High 4:2:0, 6 Baseline, 6 Extended, 9 High10 (4:2:0
  10-bit), 28 High 4:2:2 (8 and 10 bit), 13 High 4:4:4 variants, 2 High 4:0:0 (monochrome, e.g.
  `HPCVMOLQ_BRCM_B`, `HPCAMOLQ_BRCM_B`).
- Field/PAFF/MBAFF streams (all 4:2:0 8-bit): PAFF `CVPA1_TOSHIBA_B`, `CAPA1_TOSHIBA_B`,
  `cabac_mot_picaff0_full`, `cavlc_mot_picaff0_full_B`, `Sharp_MP_PAFF_*`, `camp_mot_picaff`; field-only
  `CVFI1_*`, `CVFI2_*`, `CVNLFI*`, `CAFI1_SVA_C`, `FI1_Sony_E`, `Sharp_MP_Field_*`, `cabac_mot_fld0_full`,
  `cavlc_mot_fld0_full_B`, `CVMP_MOT_FLD_L30_B`; MBAFF `CAMA*`, `cama*_vtc`, `CAMANL*`, `CAMASL3`,
  `camp_mot_mbaff*`, `cabac_mot_mbaff0_full`, `CVMA*`, `CVMANL*`, `HCAMFF1_HHI_B`; PAFF+MBAFF
  `CAPAMA3_Sand_F`, `CVMAPAQP3_Sony_E`, `MR9_BT_B`, `camp_mot_picaff`; weighted `CVWP1/2/3/5`,
  `CAWP1/5`; multi-reference/MMCO/long-term `MR1_BT_A`…`MR9_BT_B`, `FRExt_MMCO4_Sony_B`, `HCBP2`;
  FRExt interlaced `HCAFF*`, `HCAFR*`, `HPCAFL*`, `HPCVFL*`, `freh*`, `brcm_freh*`.
- Remaining syntax boundaries: FMO needs group-map transport and measured traversal;
  data partitioning and SI remain refused by this frontend. Do not conflate ASO with
  FMO. SP switching controls do exist in the UAPI: f4450d4 passes them in research
  mode, and M031/M032 capture actual Linux hardware requests. Earlier advice not
  to trace SP because of UAPI impossibility was incorrect.

### 7.2 The front end `tests/h264ctl.h` (historical rewrite `fe6a4d0`; current `f4450d4`)
Header-only, no libc, used by the FreeBSD test **and** the Linux rig. New: field pictures (each
field is a picture; `out->second_field`, `field_pic`, `bottom_field`, `mbaff`, `mmco5`), POC types
0/1/2 for frames and fields, MMCO 1–6 for frames and fields incl. long-term fields, sliding window
for fields, ref list init for fields (8.2.4.2.2–.5, alternate parity), modification with field PicNum,
frame_num gaps (non-existing frames), redundant slices skipped, picture detection by 7.4.1.2.4,
`strict` flag (set it for FreeBSD's supported eight-bit420/mono paths; clear it in the Linux rig so
the hardware's own driver is asked about everything). DPB entries carry `fields` (which fields are
marked), `FIELD` flag, long-term flag; slice lists carry `fields` per entry (1 top, 2 bottom, 3 frame)
and `index` = DPB slot. The second field of a frame is a separate request group decoded into the
**same capture buffer** (same `ts`), the OUTPUT buffers carry `HOLD_CAPTURE_BUF` on every slice but
the last of the frame (including the last slice of the first field).
Historical rewrite validation: frame streams give byte-identical controls to the old header (37 streams); the
DPB (frame_num sets, long-term indices) agrees with ffmpeg's `-debug pict+mmco` on 170 of 181 corpus
streams including PAFF/MBAFF. That early stage had unvalidated field lists and
register guesses. **Current acceptance supersedes that gap:** M026 captures
field/MBAFF programming and implements it; M027 fixes long-term metadata; M028
finishes176 accepted streams with full Y/C comparison. `f4450d4` subsequently
adds research SP/QS parsing while preserving strict refusal and all223 prior
canonical dumps. Remaining general FMO/SP/chroma/depth are still refused.

### 7.3 The Linux rig (run; current sources are versioned)
`initramfs-h264/init_h264.c` (copy in `tools/h264-campaign/init_h264.c`) decodes the embedded streams
through `/dev/video0` one request per slice and prints one line per **frame**:
`PIC <frame> <poc> <slice_type> <fnv-of-luma> <flags>` with flags = field_pic + 2·mbaff + 4·mmco5 +
8·idr; up to 8 pictures (fields) per stream; frame height = `height_in_map_units · (frame_mbs_only ? 1
: 2) · 16`. The HV ring now holds ~32 700 register accesses (`HVMAP_VETRAP_SIZE` 0x40000 in
`microkernel/hv_addrmap.h`; the old comment in the rig says 8184): about 100 slices per boot cycle.
Streams are embedded with `initramfs-h264/mkhdr.py file[:variant] ...` → `h264s.h` (variants force a
control value the bitstream does not carry: 1 `cabac_init_idc` 2, 2/4 explicit B weights, 3 idc 1).
`h264-campaign/cut.sh <name> <n>` makes cut streams (n packets; a field is a packet).
Build chain: compile the rig with `aarch64-linux-gnu-gcc -static -nostdlib -ffreestanding -O1
-fno-tree-loop-distribute-patterns -Wall -I../initramfs/stub -I/opt/bzdos/linux-work/hdr/include -I.
-o init init_h264.c` in `initramfs-h264`, copy `init` to `linux-work/initramfs/init`, then
`cd linux-work && ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make -C linux-6.12 O=$PWD/build-min -j8
Image` (initramfs source is `initramfs/rootfs.list`). Then the versioned `tools/h264-campaign/run-cycle.py <tag>` boots the
Linux flavour of the hypervisor through TFTP, waits for `TRACE-DONE` on the console, dumps the ring
with `vetrace.py`, **restores the FreeBSD TFTP pairing and reboots the guest back** (check
`tftpboot` md5s and the guest afterwards). Outputs: `<tag>.con.txt` (console, the PIC lines),
`<tag>.trace.txt` (register accesses), logs.

### 7.4 Resume here

The current stopped-session checkpoint is **§7.5**. This section retains the
earlier campaign history; do not follow its original bring-up checklist again.

**Update 2026-10-08 — M026:** the first field trace and replay are complete,
and the FreeBSD driver now accepts fields/MBAFF. Lab evidence includes full
NV12 comparison and MV canaries, not only luma. Progressive direct8=0 requires
twice the compact MV layout. BASQP1 exposed double-counted emulation-prevention
bytes in the front end; that is fixed. HCHP3's full 250 frames pass after the
reference file is mapped instead of copied into guest memory.

The full-corpus rerun lives in `linux-work/h264-traces/corpus-full-v3/` and
`corpus-full-v3-tail/` (the second continues after an empty FMO ffprobe result;
no completed stream was silently skipped). `extended-profile/` adds the four
accepted Extended streams; `corpus-memory-retry/` reruns eleven allocation
failures after reboot (all eleven exact) and MR8 (same mismatch). M026's
`corpus-final.json` preserves all attempts: 171 exact streams, three mismatching,
57 parser refusals; 18,812/18,828 frames exact.
**M027 supersedes those residual failures:** `fbsd-media 8c87ce9` adds the
measured LONG_TERM word-2 encoding (0x11) and expands source/capture DMA defaults
to 1GiB. The full repeated run is **174/174 exact, 18,828/18,828 frames**, with
zero engine errors/timeouts and no allocation failure. Two corpus passes plus
probes finish on one guest boot (50,678 successful jobs across the module reload).
The short 256/1024 A/B both passed, so the historical fragmentation explanation
remains a hypothesis. Evidence is in M027 and `linux-work/h264-traces/m027-*`.

Independent JM output matches official YUV for MR8/MR9/MMCO4, and all 240 slice
reference identities match our front end. `11001f0` versions the JM comparator
and lab metadata probes. The isolated 0x11 lab change fixes all three clips with
intact guards; MMCO4 needs just two changed writes in its failing picture.
No Linux implementation source was used. **M028 adds monochrome** with measured
PICSIZE bit19 selection and correct weighted-header syntax: `fbsd-media 3cd9cd7`.
The complete regression is **176/176, 19,028/19,028 full Y/C frames exact**,
errors=0/timeouts=0/vbq live=0. Official mono200/200, weighted36/36,
gray/color/gray80/80 and monoMBAFF72/72 are exact. Module MD5:
`d57627e64bb4cfc21e64d264e9a19b5a`. Raw evidence:
`linux-work/h264-traces/m027-mono` and `m028-full`, archived in M028-mono.
55 strict-parser refusals remain:27 ten-bit,13 fourteen-bit,ten eight-bit422,
three FMO and two SP.444/SI are additional generated probes. M029's bounded higher-chroma probes in
`linux-work/h264-traces/m029-formats` show the PICSIZE-only extension is insufficient:
420 controls exact, 422/444 CAVLC errors / CABAC wrong pixels, all four guards intact.
Normal driver recovery is 80/80 exact; errors/timeouts remain zero at that point.
Nine subsequent direct control-injection checks all return ERROR before the engine;
`rejects=errors=9` is expected (errors includes software rejections), timeouts=0,
then native f3 is 8/8 exact. The final module is unchanged. The missing entropy/output/bit-depth/FMO/switching contracts are
listed in M029; the 55 refusals remain unimplemented, not silently omitted.
Do not redo field/MBAFF, long-term or mono bring-up.


`weighted2/` holds a partial Linux capture: 25,038 entries, first six streams
complete, BASQP1 blocked after its engine error. Partial is not successful.
The runner now saves partial evidence on a deadline, rejects concurrent console
followers and reads a short tail for completion before the full dump. The rig
uses nonblocking DQBUF and releases buffers between equal-sized streams.

Use the versioned `run-cycle.py`, `verify.py`, `export.c`, `replay.py` and
`board-corpus.py`; usage is in `tools/h264-campaign/README.md`. Stop the FreeBSD
console follower before a Linux cycle, and start it again before module tests.
Keep one Linux make under `.h264-campaign-build.lock`, check its exit code,
and restore guest `/tmp` with `board-bootstrap.sh` after recovery.

`formats1/` confirms the corrected BASQP first frame and measures SPS EINVAL
for five mono/4:2:2/10-bit probes. This is a Linux driver refusal, not a proven
hardware limitation. FreeBSD intentionally still refuses these formats.

The original checklist below is retained as history/scenario coverage, not as
instructions to redo completed steps 1–3 and 6.

1. **State of the Linux build:** `initramfs/init` is the new rig with four streams
   (`CVPA1_TOSHIBA_B.4`, `CAMA1_TOSHIBA_B.4`, `CAPAMA3_Sand_F.4`, `f3`); `h264s.h` matches. The
   `make Image` was started three times and failed because two makes ran at once; at the moment
   this was written one of them was still running or had left `build-min` half built. First action:
   make sure no `make` is running (`pgrep -a make`), then run **one** `make ... Image` to completion
   and check that `build-min/arch/arm64/boot/Image` has a new mtime.
2. Run `run-cycle.sh field1`; read `field1.con.txt` for `S_EXT_CTRLS failed`, `QBUF ... failed` (which
   tells what Linux's driver refuses), the PIC lines, and compare each frame hash with ffmpeg's decode
   of the cut stream (`-apply_cropping 0`, native pix_fmt, FNV of the luma, display order from the
   POC/flags columns; a small `verify.py` for this still has to be written — the old comparison lived in
   `h264-traces/analyze.py`).
3. If frames match: the controls for fields/MBAFF are right and the hardware does them; analyse the
   register differences with `h264-lab/h264/{tr.py,join.py}` (runs are split at the write of
   `0xec`; one run per slice) against the canonical control dump (`h264-campaign/canon.c`) to learn
   which registers/bits/SRAM words carry field_pic, bottom_field, mbaff, field POCs in the table, the
   motion-vector buffer layout for fields, the output offsets/strides when a field is written into a
   frame buffer, and the low bit of the list bytes (probably the bottom-field flag, M014-f). Then
   replay with the lab (`hlab.py`/`rp.py`, needs the same field awareness) and only then change
   `cedrus_h264.c`.
4. If frames do not match: first suspect the front end (lists, POC of the other field, `fields`
   semantics), not the hardware. Use variants of the rig to change one control at a time.
5. Scenario list to trace after the first success (fits in ~100 slices a cycle, use cut streams of
   4–8 packets, prefer small pictures): MBAFF I/P/B CAVLC and CABAC (`CAMA1_TOSHIBA_B`, `CAMA3_Sand_E`,
   `camp_mot_mbaff0_full`, `CVMA1_Sony_D`), PAFF I/P/B (`CVPA1_TOSHIBA_B`, `CAPA1_TOSHIBA_B`,
   `camp_mot_picaff0_full`), field-only (`CVFI1_Sony_D`, `Sharp_MP_Field_1_B`), PAFF+MBAFF
   (`CAPAMA3_Sand_F`, `MR9_BT_B`), temporal direct in fields/MBAFF (the B streams above), explicit
   weights in fields and B (`CVWP1/2/3/5`, `CAWP1/5`; variant 2 of the rig forces B weights), long-term
   and MMCO (`MR1_BT_A`…, `FRExt_MMCO4_Sony_B`), monochrome (`HPCVMOLQ_BRCM_B`), the unsupported formats
   (`Hi422FR1_SONY_A`, `Hi422FREXT16_SONY_A`, `PPH10I1_Panasonic_A`, `FREH10-1`): run them once to see
   whether Linux's driver accepts the controls at all. A Linux refusal alone does not prove an
   engine limitation; the FreeBSD driver keeps refusing until implementation is measured.
6. Implement in the FreeBSD driver (`sys/dev/cedrus/cedrus_h264.c`) once a scenario's registers are
   understood: accept `FIELD_PIC`/`BOTTOM_FIELD`/MBAFF in `cedrus_h264_prepare`, extend the table and
   mv buffers, set the new bits; extend `tests/cedrus_h264.c` (second field into the same capture
   buffer with hold, per-frame comparison, display order from the front end's `poc`/`idr`/`mmco5`),
   replay through the lab first (the lab is the safe place: `/dev/cedrus-lab`, `README-lab.md`).
7. Decode the whole corpus (`catalog.json` marks the supported ones: 4:2:0, 8-bit, no slice groups,
   not Extended) with the provided yuv as the reference; fix the front end where only specific
   streams fail (11 DPB disagreements with ffmpeg are open: `CABACI3_Sony_B`, `CABAST3_Sony_E`,
   `CABASTBR3_Sony_B`, `CAMASL3_Sony_B`, `MR1_BT_A`, `SL1_SVA_B`, `SVA_Base_B`, `CVFC1_Sony_C`,
   `CVNLFI1_Sony_C`, `CVNLFI2_Sony_H`, `BA1_FT_C` — several are ffmpeg concealing lost packets or
   sub-sequence streams, `MR1_BT_A` and `CABACI3` (long-term) deserve a look at MMCO).

### 7.5 Stopped-session handoff — 2026-10-08

The owner requested status and stop, then explicitly requested this handoff.
Hardware work is stopped. There is no running experiment or console follower
from this session. The console was stopped by its verified PID, not by a broad
`pkill -f`. Task **fedora-183 remains open** because full hardware-only support
has not been implemented; the claims for this documentation/research work are
released at session end. No push is authorized or performed.

**Repository state.** `freebsd-media` remains `f4450d4` (`media-v4l2`), with the
production M028 driver and unchanged module hash. No tracked source was edited
there during M035–M037; only generated campaign `__pycache__` is untracked.
`microkernel` remains `d1c6f75`; its tracked sources are unchanged, pre-existing
untracked files are retained. The running board HV is a different build, as
noted above. In `bsdOS`, this handoff and M035–M037 evidence are committed
together; unrelated lima/bzkms/build changes remain untouched. Raw working
trees under `linux-work` are not git repositories: selected reproducible
evidence is saved under this repository's `measurements/` directories.

**Last verified board state.** FreeBSD SSH/UFS restored after the sole M035
Linux cycle. Production module reloaded after all lab/vendor runs; f3 8/8,
weighted mono 36/36, mono MBAFF/CABAC 36/36 full Y/C exact. Native pictures 80,
errors/timeouts/rejects/missing_refs 0; vbq live 0. Lab enabled, intentional
8 MiB arena. Guest `/tmp` still contains modules, `cedrus_h264`, `ve_lab`,
research inputs and captures; a reboot loses them. The live `kernel`,
`bananapi-min.dtb`, and `microkernel-dbg.uimg` match `.fbsd-backup`.
Final counters and MD5s are in M037, not inferred from console silence.

**Working artifacts and entry tools.**

| Area | Working path under `/opt/bzdos` | Committed evidence |
|---|---|---|
| Minimal SP, 15 cases | `linux-work/h264-traces/m035-sp-minimal/` | `measurements/M035-sp-minimal/` |
| Linux trace, 5328 entries | `linux-work/h264-traces/m035-sp-linux/` | `M035-sp-minimal/linux/` |
| Vendor host, three versions, 27 cases | `linux-work/cedarx-research/` | `measurements/M036-cedarx/` |
| Single-MB FMO/raster, recovery | `linux-work/h264-traces/m037-fmo-one-mb/` | `measurements/M037-fmo-one-mb/` |

The Linux build currently embeds M035's 15 clips and is capped at two pictures
per stream (`H264_MAX_PICS=2`). Its one make completed successfully. Rebuild
the rig/image deliberately for another capture rather than treating this image
as the former field campaign. The external JM oracle now has only its
`MAX_NUM_SLICES` limit raised from 50 to 512 for M037; exact new hash/change is
in `oracle-limit.json`. Earlier measurements keep their historical provenance.

The vendor `fetch-sdk.py` downloads exactly nine pinned binary/API objects
without a proxy. `prepare.py` MD5-verifies source delivery and compiles on the
guest; its last invocation passed. `run-probe.py` owns the board lock and saves
every request trace. Proprietary objects/API headers are deliberately external
to this repository; do not import decoder implementation into the BSD driver.
All generated inputs, actually executed lab scripts, result logs and raw-buffer
hash inventories needed to inspect the measured claims are retained.

**Next research step, only after the owner resumes work.** Start with M037's
single-MB FMO positive as a placement control. The still missing operation is
the next-MB traversal inside an original multi-MB group slice, including
neighbour/reference semantics. Seek a documented or observed map/command
contract before changing the driver; M036's vendor plugins stop before that
slice decoding and do not supply it. Keep M033's contiguous/multi-MB FMO pair
as the separating test. A genuine new traversal observation can then be
checked with guards/full Y/C before production admission.

For switching, M035 shows failure even in explicit zero-residual inter and
zero-token I16x16 SP; do not repeat all16 slice-code sweeps from M032 or call
SP skip correct by choosing FFmpeg over the disagreeing JM. Higher-depth and
higher-chroma remain without a working register/output contract. No current
measurement turns those modes into a generic hardware-supported path. The
production acceptance remains 176/231 streams (19,028 exact frames); the
remaining 55 are accounted for, not implemented. Mono PCM is a separate caveat
outside the tested accepted corpus. A CPU decoder/reconstruction substitution
would violate the owner's explicit hardware-only requirement.

## 8. Open items outside the campaign
- Explicit B weights are now measured: CVWP2 has 58 real weighted B slices and
  all 90 frames match after the direct8=0 MV fix (M026).
- ffmpeg / GStreamer integration of the decoder (v4l2 request path) is not started.
- Display scanout of the decoded frame (the hypervisor owns DE2) is not done.
- Push: nothing is pushed. Ask the owner.

## 9. How the previous agent worked (useful habits)
- Make every claim from a measurement with a count, state the denominator, and when a conclusion
  turns out wrong write the retraction into the fact file, not only into the chat.
- Keep one A/B knob per hypothesis in the driver while measuring, then remove them in a separate
  commit once the cause is known (the M016/M022 knobs were removed after M023).
- Fact files are numbered `Mnnn-*.md`, each finding `F:Mnnn-x`; superseded ones get a banner on top,
  the text stays.
- Put scripts that found something into the repo (`tools/`), not only into a scratch directory: the
  session scratchpad is deleted.
