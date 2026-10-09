# Commit map

> **2026-10-09:** the history was rewritten once more (text scrub, tool-specific files removed). Hashes quoted in
> the notes and in the table below are those of the first import; the map from the hashes of that import to the current ones is
> `COMMIT-MAP-2026-10-09.txt` (two columns: old, new). The branch is `master`. A last rewrite on 2026-10-10 (one more text fix) changed the hashes again: `COMMIT-MAP-2026-10-10.txt` maps the hashes of the 2026-10-09 history (old, new).


The repository was created on 2026-10-08 from the `media-v4l2` branch of the old `fbsd-media` worktree
(only the paths of this repository, committers normalised to Andrey Bodrov), so every hash changed. Facts and
handoff files quote the old hashes; the new ones are below (matched by subject, in order).

| old | new | subject |
|---|---|---|
| `d3650a0` | `2a04292` | media: V4L2 UAPI headers and the M0 ABI test |
| `ca49923` | `e0bd287` | media: V4L2 core (vdev, vbq, vctrl) and the vtest capture driver |
| `ef07b41` | `5f851a2` | media: events, priorities, CREATE_BUFS; v4l2-compliance 45/45 without streaming |
| `3769943` | `919bef4` | media: v4l2-compliance -s 54/54; unload safety; leak-checked |
| `c44d175` | `8e23d90` | media: fresh timestamp per frame; vtest holds its frame rate |
| `ac1b330` | `64df3c8` | media: queues belong to open files; pages memory mode; OUTPUT queue semantics |
| `a498a40` | `a5789a1` | media: mem2mem (vm2m scheduler, vconv converter); v4l2-compliance -s 54/54 on both devices |
| `f1cb33a` | `eb023e5` | dmabuf: native core; V4L2 EXPBUF and DMABUF import; orphaned buffers |
| `95554cd` | `199a833` | media: XRGB8888 in vtest; tests that take a V4L2 buffer into the GPU |
| `0e4a4df` | `bb554ac` | media: media controller, request API, array and compound controls (M3) |
| `18007e3` | `90cc527` | cedrus: A64 video engine bring-up probe |
| `a4681e3` | `44fa2c4` | cedrus: /dev/cedrus-lab (registers, uncached contiguous buffer) and the ve_lab script tool |
| `f634025` | `c3ebb1b` | cedrus: lab sysctl to write system-control register 0 |
| `ac4553e` | `6e7b4cb` | cedrus lab: uncached buffer mapping, batched register writes, physical poke |
| `5abe929` | `26aec55` | cedrus lab: kernel alias uncached too (no memattr warnings), fall back from low to full window, NOWAIT alloc |
| `3f20847` | `f1c246f` | cedrus: hw.cedrus.lab_size tunable for the lab buffer |
| `193ca0f` | `06cc09f` | cedrus: mark the lab buffer pages uncached; ve_lab sync command |
| `60ec8c8` | `9499139` | cedrus: stateless MPEG-2 V4L2 decoder (intra pictures) |
| `3c46e52` | `39003ad` | cedrus: P and B pictures; vbq_find_timestamp for reference lookup |
| `4c4492b` | `27b3f31` | cedrus_gpu: decode, export, import as NV12 in Mesa, draw; dmabuf export size is a page multiple |
| `2dc7bcf` | `42de64e` | cedrus: gate the engine when idle (power references, 200 ms idle timer) |
| `af294d0` | `48c5748` | cedrus: field pictures; m2m HOLD_CAPTURE_BUF |
| `4f4167e` | `7e86016` | cedrus_kms: decoded video to the screen through GPU import and KMS page flips |
| `f2566e5` | `1be8d4c` | tests: h264ctl.h, a userland H.264 front end producing the V4L2 stateless controls |
| `26e72f5` | `9fc212b` | cedrus: H.264 stateless decoder, and the test that checks it |
| `8489c1d` | `814a0e6` | media: run-h264-test.sh, the board run the H.264 test needs |
| `a47da73` | `b93e309` | cedrus: the MV halves must follow the picture stride (F:M014-k) |
| `e970183` | `ac52a07` | cedrus: instrumentation for the intermittent H.264 failure |
| `d9c69ea` | `55d97e5` | cedrus tests: tell a wrong picture from a stale read; gate the reference read |
| `88986b0` | `5607e3d` | cedrus: frame hash through the direct map, and the buffers' memory attribute |
| `e1c568f` | `703979c` | cedrus: the MV halves are wmb*hmb*16 apart, as Linux programs them (F:M017-a) |
| `27f8652` | `855635a` | cedrus: let the auxiliary allocation reach 1 GiB, so 1280x720 decodes |
| `20d2ef1` | `f323431` | cedrus: knobs to bisect the MV and scratch buffer sizes |
| `5191343` | `ddc54b7` | media: board-bootstrap.sh, put the test rig back on the board in one go |
| `c6cca28` | `2f9f1de` | cedrus tests: stop feeding a picture whose slice failed |
| `103718c` | `4c66cdf` | cedrus: knob to take the engine's buffers from the picture's segment |
| `9f651ed` | `42758f0` | cedrus: the scratch buffers need a quarter of what the formula asked |
| `fca21a3` | `b70e06b` | cedrus: knob to ask the clock framework for another engine clock |
| `1e71844` | `e896669` | cedrus h264: wait for the bit stream reader after a skip, not for bit 16 (F:M023) |
| `d649f66` | `552aff4` | cedrus tests: do not decode a picture into a buffer it still references |
| `b772668` | `ffd091a` | cedrus h264: remove the experiment knobs of M016/M022 |
| `4ebf1cb` | `87e4b6b` | vm2m: complete the slices of a picture whose earlier slice failed |
| `74f6a69` | `f700733` | cedrus tests: -k keeps feeding a failed picture; fewer capture buffers when memory is short |
| `fe6a4d0` | `82234c4` | h264ctl: fields, MBAFF, POC types 0-2, MMCO and long-term for fields, frame_num gaps; campaign toolbox |
| `a7dcb59` | `7cfc223` | tools: automate H264 board capture and verified FreeBSD recovery |
| `1860bca` | `41fd57f` | cedrus: decode H.264 fields and MBAFF with measured MV layouts |
| `11001f0` | `596a89e` | media: compare H264 reference lists with JM and probe long-term metadata |
| `8c87ce9` | `5f5c5c8` | cedrus: encode H264 long-term references and widen data DMA placement |
| `3cd9cd7` | `b04ec5b` | cedrus: decode measured monochrome H264 and parse its weighted syntax |
| `f4450d4` | `1e562ca` | media: parse SP switching controls for hardware research |
