# freebsd-media

A clean-room, BSD-2-Clause V4L2 stack for FreeBSD — core, dma-buf, media controller, request API,
mem2mem scheduler — and a driver for the Allwinner A64 video engine ("Cedrus") that decodes
MPEG-2 and H.264 through the stateless V4L2 interface, with zero copy to the GPU (Mali-400 / lima)
and to KMS. Target hardware: Banana Pi M64 running FreeBSD 15.1 arm64 as a guest of the
[bzdk](https://github.com/bzdOS/bzdk) hypervisor.

The layout mirrors the FreeBSD source tree, so the directories drop into `/usr/src`:

| Path | What |
|---|---|
| `sys/dev/media` | V4L2 core (`vdev`, `vbq`, `vctrl`, `vevent`, `vm2m`, `vreq`, `vmedia`), test drivers `vtest` (camera) and `vconv` (converter), the UAPI headers (`uapi/`; see `docs/AUDIT.md` for their licences), tests and tools |
| `sys/dev/dmabuf` | native dma-buf |
| `sys/dev/cedrus` | the A64 video engine: bring-up, MPEG-2 and H.264 decoders, the register lab |
| `sys/modules/{media,cedrus,dmabuf,vtest}` | module Makefiles |
| `sys/dev/media/tests` | userland tests, incl. `h264ctl.h` (an H.264 front end that makes the stateless controls) |
| `sys/dev/media/tools` | cross-build (`xbuild.sh`), board bring-up (`board-bootstrap.sh`), compliance, regression scripts |
| `docs/cedrus-facts` | the measurements (M001–M038) the driver is built from, and `HANDOFF.md` / `HANDOVER.md` |
| `docs/specs` | design spec |

## Provenance notes

- The V4L2 core deliberately follows the method names of Linux's vb2 / m2m framework (`queue_setup`, `buf_queue`,
  `device_run`, `job_finish`, ...), because those are the names driver authors of the V4L2 ecosystem expect. The code is
  our own; the names are interface vocabulary.
- `uapi/linux/media.h` is generated from `uapi/media.spec`; see `sys/dev/media/uapi/README`.
- The repository history was rewritten when this work was moved here from its earlier worktree (old to new hashes in
  `docs/cedrus-facts/COMMIT-MAP.md`).
- The excerpts of JVT conformance streams that once sat in `docs/cedrus-facts/measurements/M028-mono` and `M031-sp`
  were removed (source terms unchecked); the measurement notes name the streams, which come from the public suite.

## Status (2026-10-08)

- V4L2 core: `v4l2-compliance` 53/53 on the test camera and converter, 46/46 on Cedrus.
- MPEG-2: I/P/B frames and interlaced frame pictures, within ±2 per sample of ffmpeg's decode (the IDCT is not
  bit-exact, M038); one long clip drifts a little further.
- H.264: frames, field pictures (PAFF), MBAFF, long-term references, explicit weights, 8-bit monochrome:
  176 of the 231 streams of the JVT conformance suite (the 55 others use 10/14 bit, 4:2:2, FMO or SP/SI and are refused
  by the driver), 19,028 of 19,028 frames identical to the reference decode (JM/FFmpeg) on the board (HANDOVER.md §7).
  10/14 bit, 4:2:2/4:4:4, general FMO and SP/SI did not work in our measurements (M029–M037); that is not proven to be
  a silicon limit.
- FFmpeg (Kwiboo's v4l2-request branch, `-hwaccel v4l2request`) decodes through the driver: M039. Decoded frames reach the screen without a CPU copy (EGLImage on lima, bzkms page flips) at 57-60 fps up to 1080p; decode runs at 82 fps at 1080p using about 0.3 s of CPU per 150 frames (M040). GStreamer is not done.

## Clean room

The register programming was found by observing an unmodified Linux 6.12 guest (the hypervisor logs every
access to the engine's registers) and by experiments on the board; no Linux driver source was read or
copied. The limits of that claim (the author is a language model with training-data exposure, the register-name
table comes from a wiki, one UAPI header is a trimmed copy) are in `docs/AUDIT.md`. The Linux UAPI headers under `sys/dev/media/uapi` are the published interface.
The code was written with AI assistance and reviewed by the owner. FreeBSD's own policy on such
contributions (it differs between `main` and the release branches) applies to anything proposed upstream; this repository is a standalone out-of-tree project.

## Build and test

Cross-build against the board's kernel headers: `LC_ALL=C sys/dev/media/tools/xbuild.sh cedrus`
(needs a FreeBSD source tree, see `docs/cedrus-facts/HANDOVER.md`). On the board:
`sys/dev/media/tools/board-bootstrap.sh`, then `/tmp/ve/cedrus_h264` / `cedrus_mpeg2`. The guest is
tmpfs-based: modules load from `/tmp` after every boot.

## License

BSD-2-Clause (see `LICENSE`; files carry SPDX identifiers).
