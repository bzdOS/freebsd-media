# Native V4L2 for FreeBSD: architecture

Status: **draft, 2026-10-05.** Nothing is implemented yet. This document fixes
the decisions that everything else depends on. The open questions are listed
at the end.

## 1. Goal

FreeBSD has no in-kernel video framework at all: no V4L2 core, no buffer queue,
no media controller, no request API. The only V4L2 on FreeBSD is `webcamd`,
which runs Linux USB webcam drivers in userland over CUSE. Hardware video
decode and SoC cameras on arm64 are therefore impossible: Allwinner Cedrus,
Rockchip rkvdec/hantro, sun6i-csi and others all need that framework.
(`microkernel/docs/cedrus-assessment.md`, `csi-camera-assessment.md`.)

The goal is a V4L2 implementation that lives in FreeBSD base:

- **Outside, exactly V4L2.** The same ioctls and structures as Linux, so
  ffmpeg, GStreamer, v4l-utils and browsers build and run unchanged.
- **Inside, native FreeBSD.** newbus, busdma, devfs, kqueue, kobj; BSD-2
  licence; no Linux code copied.
- **Modern subset only.** See §3.

## 2. Why not port Linux, and why not a subtree

- **Licence.** Linux `drivers/media/v4l2-core`, `mc` (including the request
  API) and `common/videobuf2` are GPL-2.0. So are `cedrus`, `hantro` and
  `uvc`. GPL code does not go into the FreeBSD kernel, so a LinuxKPI port
  would stay an out-of-tree port forever. drm-kmod can live that way only
  because the DRM core is MIT.
- **A second compatibility layer is a trap.** `drm-subtree`
  (github.com/evadot/drm-subtree) imported the MIT DRM core into
  `sys/dev/drm` with its own `drmkpi` shim instead of LinuxKPI. It stopped in
  2022 (last commit 2022-05-03) and never reached base. Tracking Linux
  internals through a private shim did not survive with one or two people.
- **A native core has no tracking burden.** The V4L2 UAPI is stable and has
  an external conformance test (`v4l2-compliance`). We follow the API, not
  Linux internals. The cost is in the drivers, which are written natively,
  using Linux and public documentation as a reference for the hardware.

The UAPI headers can be used as they are: `videodev2.h` and `v4l2-controls.h`
are dual-licensed `(GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause`.
`media.h` and `v4l2-subdev.h` are GPL-2.0 with the syscall note. Userland
already ships them as `multimedia/v4l_compat` (from v4l-utils 1.23.0).
Whether base installs them, or a BSD-licensed `media.h` is written from the
documented interface, is open question Q1.

## 3. Scope

**In scope**

| Area | What |
|---|---|
| Video nodes | `/dev/videoN`: capture, output, mem2mem; single- and multi-planar formats |
| Memory | `V4L2_MEMORY_MMAP` and `V4L2_MEMORY_DMABUF` (import and `VIDIOC_EXPBUF` export) |
| Controls | the controls framework, including compound controls (stateless codec parameters) |
| Events | `VIDIOC_DQEVENT`, source change, EOS, control change |
| mem2mem | a helper for decoders, encoders and converters |
| Media controller | `/dev/mediaN`: entities, pads, links, topology |
| Request API | `MEDIA_IOC_REQUEST_ALLOC`, request fds, controls and buffers bound to requests |
| Subdevices | `/dev/v4l-subdevN`, pad formats, for sensors and CSI bridges |
| Waiting | `poll(2)`, `select(2)`, `kqueue(2)` |

**Not in scope (legacy)**

- V4L1 and `videobuf1`.
- `read()`/`write()` I/O and `V4L2_MEMORY_USERPTR`: MMAP and DMABUF cover
  every current consumer.
- Overlay, analog TV, tuners, radio, VBI, DVB.
- compat32 (32-bit userland on a 64-bit kernel).
- Linux binary compatibility. Userland is built against FreeBSD headers;
  see §6.

## 4. Layering

```
 ffmpeg / GStreamer / v4l2-ctl / v4l2-compliance / browsers
        |  open/ioctl/mmap/poll/kqueue        (V4L2 + media UAPI)
 -------+-----------------------------------------------------------
 /dev/videoN        /dev/mediaN        /dev/v4l-subdevN
   vdev  -- ioctl dispatch, argument validation, file handles,
   |        events, priority, poll/kqueue
   ctrl  -- control handlers, ranges/menus, compound payloads
   vbq   -- buffer queue: states, ownership, done list, streaming
   |        mem backends: contig (busdma), sg (busdma); dmabuf in/out
   m2m   -- job scheduling between an OUTPUT and a CAPTURE queue
   mc    -- graph (entities/pads/links), requests, topology ioctls
   subdev-- pad ops, formats, routing
 -------+-----------------------------------------------------------
        |  kobj interfaces (vdev_if.m, vbq_if.m, subdev_if.m, ...)
 drivers: vtest (test driver), aw_ve (Cedrus), uvc, rk_vdec, sun6i_csi, sensors
        |  newbus, busdma, clk, hwreset, regulator, ofw
 hardware
```

Source layout (proposal): `sys/dev/media/` for the core, one directory per
component (`vdev/`, `vbq/`, `ctrl/`, `m2m/`, `mc/`, `subdev/`), test drivers in
`sys/dev/media/test/`, SoC drivers next to their SoC (`sys/arm/allwinner/aw_ve/`).
Modules: `media.ko` (the whole core), one module per driver.

## 5. Driver interface

Drivers implement **kobj interfaces**, not function-pointer tables:

- it is the FreeBSD idiom (`device_if.m`, `clknode_if.m`, the in-base
  `crtc_if.m`);
- default methods give "not supported" answers for free, so a driver
  implements only what it has;
- the interfaces are versioned by the `.m` files.

The **semantics** of the methods deliberately follow Linux's
`v4l2_ioctl_ops`, `vb2_ops` and `v4l2_m2m_ops`: `queue_setup`, `buf_prepare`,
`buf_queue`, `start_streaming`, `stop_streaming`, `device_run`, and so on.
Reasons:

- a Linux driver used as a hardware reference maps method by method;
- if a LinuxKPI glue for GPL drivers is ever wanted, it can be a thin out-of-tree
  layer on top of these interfaces. The core never depends on it.

The core does all generic validation (buffer indices, types, field values,
format sanity, state transitions) before calling into a driver. A driver
sees only arguments that already passed.

## 6. ioctl ABI

- ioctl numbers are produced by FreeBSD's `<sys/ioccom.h>`. They are not
  Linux's numbers. This is the same ABI that `multimedia/v4l_compat` already
  gives userland, so ffmpeg, v4l-utils and friends built from ports keep
  working, and the kernel accepts exactly what they send. This is checked
  against the port headers in M0.
- FreeBSD encodes the argument size in 13 bits (`IOCPARM_SHIFT`), Linux in 14.
  Every V4L2 and media structure has to fit in 8191 bytes, and a size check is
  part of the M0 header test.
- Pointers inside structures (`v4l2_ext_controls.controls`,
  `v4l2_plane.m.userptr`, payloads of compound controls) are copied with
  `copyin`/`copyout` by the core, never by drivers.
- 64-bit userland only.

## 7. Memory

**Allocation.** MMAP buffers are allocated with `bus_dmamem_alloc()` on a tag
derived from the device (`bus_get_dma_tag(dev)`), with `BUS_DMA_COHERENT`
where the driver asks for coherent memory. busdma then chooses the right
memory attribute per device: uncacheable on a non-coherent tag, write-back
on a coherent one. (This is exactly the mistake LinuxKPI's
`dma_alloc_coherent()` makes and that we already hit with lima.)

- **contig backend:** one physically contiguous allocation. Needed by Cedrus
  and most SoC codecs without an IOMMU.
- **sg backend:** pages plus a busdma map with several segments, for devices
  with an IOMMU or scatter-gather DMA.

**Cache maintenance** happens at the queue boundaries: `bus_dmamap_sync()`
PREWRITE/PREREAD on `QBUF`, POSTREAD on completion. Userland mappings of
non-coherent buffers are uncacheable or synced, never silently cacheable.

**mmap.** `VIDIOC_QUERYBUF` returns an offset. `d_mmap_single` hands out a
`vm_object` backing that buffer (an `OBJT_SG` object over the busdma segments).
The buffer stays alive while any mapping or dma-buf reference exists.

**dma-buf.** Sharing buffers with DRM is the point of the exercise: decoded
frames go to lima as EGL images and to KMS planes without a copy. FreeBSD base
has no dma-buf. drm-kmod has one (`drivers/dma-buf/`), and most of it is BSD
or MIT:

| File in drm-kmod | Licence |
|---|---|
| `dma-buf.c` (DTYPE_DMABUF file type) | BSD (Matt Macy) |
| `dma-fence.c`, `dma-fence-array.c`, `dma-fence-chain.c`, `dma-buf-kmod.c` | BSD (Beckhoff) |
| `dma-resv.c` | MIT |
| `sync_file.c`, `dma-fence-unwrap.c` | **GPL-2.0** |

**Prerequisite P1:** move dma-buf, dma-fence and dma-resv into base as a
native subsystem (`sys/dev/dmabuf/` or `sys/kern/`), and re-implement
`sync_file` and `dma-fence-unwrap` under BSD. drm-kmod then uses the base one,
and the media core imports and exports through the same interface. That is
an upstream change of its own, and it benefits drm-kmod too.

## 8. Requests and stateless codecs

- `MEDIA_IOC_REQUEST_ALLOC` creates a request object and returns its fd
  (`falloc`, its own `DTYPE`).
- Control values (`VIDIOC_S_EXT_CTRLS` with `V4L2_CTRL_WHICH_REQUEST_VAL`) and
  buffers (`QBUF` with `V4L2_BUF_FLAG_REQUEST_FD`) attach to the request.
- `MEDIA_REQUEST_IOC_QUEUE` validates the whole request and hands it to the
  m2m scheduler. Completion is signalled through `poll`/`kqueue` on the
  request fd. `MEDIA_REQUEST_IOC_REINIT` recycles it.
- A stateless decoder's `device_run` gets the request, reads its controls
  (SPS, PPS, slice params, decode params) and the buffers, programs the
  hardware, and completes from the interrupt.

## 9. Concurrency and lifetime

- **ioctl serialisation:** one `sx` lock per video device by default, which
  is what nearly every Linux driver relies on. Drivers can opt into finer
  locking later.
- **vbq:** a mutex protects queue state and the done list. Completion runs
  from the interrupt handler or a taskqueue. Waiters use `sleep`/`wakeup`,
  `selrecord`/`selwakeup` and `KNOTE`.
- **Detach with open files:** device nodes go away with `destroy_dev_sched()`,
  and every object (device, file handle, buffer, request) is reference counted.
  After detach, every ioctl returns `ENODEV`, and the memory is freed when the
  last mmap and dma-buf reference is gone. (We already paid for getting this
  wrong once: the drm-kmod `drm_dev_alias` leak, PR #514.)

## 10. The chimp board

**The current display ownership stays.** The hypervisor keeps DE2/HDMI and the
guest gets `bzkms`. Video decode does not touch the display: Cedrus writes into
a dma-buf, lima imports it, bzkms flips it, the hypervisor scans out guest
memory as it already does for the zero-copy path. A native DE2 driver
(`aw_de2*` from drm-subtree, BSD-2) is a separate task for FreeBSD on bare
metal, on other boards.

Things to check on hardware before aw_ve:

1. **VE MMIO** (`0x01c0e000`) and its interrupt (SPI 58) reach the guest. The VE
   node is in the guest DTB already. It is not in `board-config.xml`.
2. **SRAM C1 claim.** VE maps SRAM C1 through syscon `0x01c00000 + 0x0`
   (bits 0..30). The hypervisor writes `+0x4` bit 24 to give SRAM C to DE2
   (`hdmi.c:479`). Different registers in the same 4 KiB page. Both sides must
   write their own register only, never a read-modify-write of the other.
3. **Clocks and reset** (`ahb`, `mod`, `ram` gates, VE reset) go through the
   CCU page, which the hypervisor traps for the watchdog. Check that guest
   writes to the VE gates pass through.
4. **DMA addresses.** Guest physical must equal machine physical for VE DMA,
   as it already does for lima. Confirm, and check the VE's address range
   limits on A64 against where guest RAM sits.

## 11. Milestones

Every milestone ends with `v4l2-compliance` clean for what it adds.

| | Contents | Exit criterion |
|---|---|---|
| M0 | UAPI headers in base; ABI test against the Linux UAPI | every ioctl has the same direction, group, number and argument size on FreeBSD as in Linux, all sizes ≤ 8191. **Done 2026-10-05**, see below |
| M1 | vdev + vbq (contig, MMAP) + ctrl + events + priorities; `vtest` capture device producing test patterns | `v4l2-compliance -d /dev/video0 -s` passes; `ffmpeg -f v4l2` records. **Done 2026-10-05**, see below |
| M2 | m2m; `vtest` gains a software mem2mem converter | `v4l2-compliance` m2m tests pass (events moved into M1). **Done 2026-10-05**, see below |
| P1 | dma-buf in base; a bridge to drm-kmod. **Done 2026-10-05** | lima renders from an imported vtest buffer: the GPU sampled the camera's buffer through a dmabuf, all bars right |
| M3 | mc + requests + compound controls; a stateless-shaped test codec | request tests in `v4l2-compliance` pass |
| M4 | `aw_ve`: Cedrus on A64, MPEG-2 then H.264 then HEVC | ffmpeg v4l2-request decodes test streams bit-exact against software decode |
| M5 | subdev + sensor/CSI; UVC in kernel; Rockchip decoders | per driver |

M0–M3 need no special hardware and run on chimp as a plain FreeBSD guest.

**Where the code is:** worktree `/opt/bzdos/freebsd-media`, branch `media-v4l2`
(from FreeBSD main `a5b1b2d`), under `sys/dev/media/`. Not pushed anywhere yet.

**M0 result (2026-10-05, commit `d3650a0`).** `tests/abi/gen_abi.py` checks the
118 `VIDIOC_*`/`MEDIA_*` ioctls of `videodev2.h`, `media.h` and
`v4l2-subdev.h` from Linux 6.12. The Linux values come out the same natively
(x86_64) and from an aarch64 object. On the FreeBSD side (clang, aarch64
sysroot, `<sys/ioccom.h>`) all 118 match; no argument is larger than 8191
bytes. The test fails when it should: changing one expected size, direction,
number or group each produces its own static-assert error.

**M1 result (2026-10-05).** `sys/dev/media/`: `vdev.c` (node, ioctl dispatch,
priorities), `vbq.c` (buffer queue, MMAP), `vctrl.c` (controls), `vevent.c`
(events), `vtest.c` (virtual camera: YUYV and RGB24, 160x120..1920x1080, five
frame rates, six controls). Built out of tree with `tools/xbuild.sh` against the
guest kernel's headers; `tools/guest-reload.sh` pushes (md5 verified) and
reloads on the board, `tools/compliance.sh` runs v4l2-compliance there.

On the board `v4l2-compliance -s` passes 54 of 54 (MMAP with no poll, select
and epoll), `ffmpeg -f v4l2` records exactly 3.00 s, and the frame rate is
exact (180 frames in 6 s at 30 fps). Unloading `vtest` under a running stream
ends the client with ENXIO and no panic; the module refuses to unload while a
mapped buffer is alive. Three compliance runs, five streams and a SIGKILLed
streamer leave no allocations behind.

What running the real tests taught, each fixed:

- **FreeBSD does not copy an ioctl's result out when the handler returns an
  error** (`kern_ioctl`, `sys_generic.c`: `error == 0 && (com & IOC_OUT)`).
  V4L2 returns data with errors: `error_idx` of `VIDIOC_*_EXT_CTRLS`, and the
  size a too-small payload needs. The ext-controls handler copies the
  structure out itself, to the address `ioctl(2)` was called with (the saved
  third syscall argument), and only when the call really is that syscall. It
  is a workaround, not a clean design; a proper fix is a flag in the cdevsw
  that asks the framework to copy out on error, which would be an upstream
  change of its own. Compound controls will need it more.
- **v4l2-compliance cannot see everything.** A recycled buffer kept the
  timestamp of its previous frame and compliance passed, but ffmpeg's input
  stalled. A real consumer is part of the exit criteria for every driver.
- A dequeued buffer must not carry `V4L2_BUF_FLAG_DONE`; QUERYBUF ignores the
  `memory` field of its input; `max_num_buffers` in `v4l2_create_buffers` is
  `reserved[0]` to older userland and must stay zero; `NEXT_CTRL` and
  `NEXT_COMPOUND` select different kinds of control; a device with controls
  must support control events.

Not done yet, by design: DMABUF import and `VIDIOC_EXPBUF` (needs P1),
multi-planar formats, compound controls, `V4L2_MEMORY_USERPTR` (never),
a media device node, the request API.

**M2 result (2026-10-05).** `vm2m.c` is the scheduler (a job is one source
buffer turned into one destination buffer; driver methods in `vm2m_if.m`),
`vconv.c` the test converter (a second driver in `vtest.ko`: YUYV and RGB24 in
either direction, mirror controls, per-file formats, queues and controls). To
make that possible the core was refactored: queues, controls and driver state
now belong to open files (`vdev` methods take the file; `fh_open`/`fh_close`),
the ioctls find the queue through the file and the buffer type, and
`vdev_unregister()` takes open files apart itself. `v4l2-compliance -s` passes
54 of 54 on both `vtest` and `vconv`; a camera frame through the converter
gives exactly the bar colours, mirrored by the flip controls, at 38 fps for
1280x720 in software; two clients at once, SIGKILLed clients and an unload
under a running stream leave nothing behind.

Found by running it (each fixed, two of them took the guest down or hung a
client, which a compliance pass alone would not have shown):

- **FreeBSD allows two `selrecord()` calls per thread per scan.** A device with
  two queues and events has three sources; the first version recorded all three
  and panicked the guest. One `selinfo` per device, recorded once before the
  readiness checks, woken by every state change. kqueue is not limited.
- **`POLLERR` for a mem2mem file means both queues are unusable.** One queue
  idling is normal; reporting it as an error made `v4l2-ctl` spin.
- **A stateless way of thinking about memory was wrong for software devices.**
  CREATE_BUFS twice the size of 1920x1080 RGB24 is 12 MB of physically
  contiguous memory, which fails on a fragmented machine and looked like a
  flaky test (11 runs of 12) until the state left by the first run was found.
  A queue can now use separate pages (`pages`), the `sg` backend of this
  document in its simplest form.
- **v4l-utils on FreeBSD only recognises capture devices** (`mi_media_detect_type`
  tests `V4L2_CAP_VIDEO_CAPTURE` and nothing for output or mem2mem). The test
  tools are patched in place (`tools/v4l-m2m-patch.py`); the port needs the real
  fix, which is a few lines in that function.

**P1, first half (2026-10-05).** `sys/dev/dmabuf` (module `dmabuf_base`) is the
native dma-buf core: an exporter wraps its memory in a `struct dmabuf` with
`mmap`, `vmap`, `sglist` and `release` operations, userland gets a descriptor of
a file type of its own, an importer turns the descriptor back into the buffer.
In `media`, `VIDIOC_EXPBUF` exports a MMAP buffer, `V4L2_MEMORY_DMABUF` imports
one, and `V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS` is claimed (freeing a queue is no
longer blocked by a mapping or an exported descriptor; the memory lives until
the last user lets go). `v4l2-compliance -s -e <other node>` passes 54/54 on
both devices with the DMABUF tests; `tests/zerocopy.c` runs 200 frames from the
camera through a descriptor into the converter with every bar right and no copy;
`tests/dmabuf_life.c` shows an exported buffer outliving its queue, its device
and its driver's unload.

**P1, second half: the GPU side (2026-10-05).** drm-kmod's PRIME import calls its own
`dma_buf_get()`, which only knew its own descriptors. Patch `0006` in
`hal/lima/patches/drm-kmod` (local, not upstream: it needs the native core) makes
`dma_buf_get()` fall back to a bridge in `dma-buf-native.c`: the native buffer is
wrapped in a `struct dma_buf` whose `map_dma_buf` builds a DMA-mapped `sg_table`
from the native physical segments and whose `release` drops the native
reference; `dmabuf.ko` now depends on `dmabuf_base`. On the board:
`tests/prime_import.c` imports a V4L2 capture buffer into lima through
`DRM_IOCTL_PRIME_FD_TO_HANDLE` (twice, closing both handles), and
`tests/prime_texture.c` has Mesa import the descriptor as an `EGLImage`
(`EGL_LINUX_DMA_BUF_EXT`, XRGB8888), sample it with a textured quad into a
render target and read it back: the Mali-400 drew the camera's own buffer, all
eight bars right, no GPU hang. `vtest` got `XBGR32` ('XR24', the same fourcc as
DRM's XRGB8888) for it.

Known limits of the bridge: every `dma_buf_get()` of a native descriptor makes a
new wrapper, so importing one descriptor twice gives two handles (no dedup);
there is no `mmap` or `vmap` on the wrapper, so a GPU driver cannot map an
imported buffer for the CPU; only pages the VM manages can be mapped; and nothing
synchronises the CPU and the GPU (no fences, no `DMA_BUF_IOCTL_SYNC`; the
`dma_map_sgtable()` does the cache maintenance at map time). `dmabuf_base.ko` and
the bridged `dmabuf.ko` are loaded by hand from `/tmp` for now; shipping them
means putting `dmabuf_base.ko` in `/boot/modules` next to the new `dmabuf.ko`.

**M4: the decoder and the GPU (2026-10-05).** The Cedrus driver (`sys/dev/cedrus`) is a
stateless MPEG-2 decoder on the M3 request API: I, P and B frame pictures decode
exactly against ffmpeg (facts M009-M012 in `docs/cedrus-facts`). Its NV12 capture
buffers are DMA segments the engine writes directly (below 256 MiB of DRAM:
`vbq.dma_maxaddr`). `tests/cedrus_gpu.c` decodes a picture, exports the capture buffer
with `VIDIOC_EXPBUF`, and Mesa imports it as a two-plane NV12 `EGLImage`
(`EGL_LINUX_DMA_BUF_EXT`, plane 1 at `stride * height`) and draws it through an
external texture into a render target: every pixel equals a CPU BT.601 conversion of the
same buffer (pictures 192x144, 176x144, 100x60). No copy anywhere between the engine
and the GPU. The Mali-400 takes the NV12 import as is (Mesa converts in the shader);
R8/GR88 per-plane imports are refused. The bridged `dmabuf.ko` must be the loaded one:
a guest reboot brings back the stock `/boot/modules/dmabuf.ko`, and then the PRIME
import fails with EINVAL (swap: unload bzfb, lima, drm, dmabuf; load `/tmp/dmabuf.ko`;
load drm, lima, bzfb).

**M4: the screen (2026-10-05).** `tests/cedrus_kms.c` plays a stream in a loop: every
decoded capture buffer is imported once as an NV12 `EGLImage`, a textured quad draws it
(aspect kept) into a gbm scanout surface on lima, the buffer goes to bzkms
(`drmPrimeFDToHandle`, `drmModeAddFB2`) and is page-flipped, each flip waited for. 352x288
MPEG-2 from the video engine on the 1120x276 guest window: 480 frames in 8 s = 60.0 fps,
480 presented, 0 refused, no decoder timeouts. The decoder and the GPU never copy the
picture; the engine writes it, the GPU samples it, the display engine scans out the
GPU's buffer. (bzkms and bzfb exclude each other: unload bzfb, load bzkms.)

Corrections to what this document said before M0:

- The raw ioctl numbers are **not** the same on both systems: the direction
  bits are swapped and the size field is 13 bits against 14. Only the tuple
  (direction, group, number, size) is the same, and that is what userland
  compiled against FreeBSD headers needs. The test compares the tuple.
- The reference is the Linux 6.12 UAPI, not the `v4l_compat` port. That port
  installs the stock Linux headers (v4l-utils 1.23.0) and only adds
  `linux/types.h` for FreeBSD, so the ABI is the same thing.
- Only the three dual-licensed headers are in the tree (Q1 stays open for the
  GPL-only ones).

## 12. Open questions

- **Q1. Headers in base.** Install the dual-licensed `videodev2.h`/`v4l2-controls.h`
  and write a BSD `media.h`/`v4l2-subdev.h` from the documented interface, or
  depend on `v4l_compat` for userland and keep kernel-private copies?
- **Q2. Coexistence with webcamd.** Both create `/dev/videoN`. Pick a policy:
  kernel nodes start after webcamd's, or webcamd is skipped for devices a
  kernel driver owns.
- **Q3. Clean-room discipline for drivers.** Cedrus has no public datasheet,
  only the linux-sunxi documentation and the GPL driver. Decide how register
  knowledge is recorded (fact sheets of registers and sequences, written up
  separately from the code) so the BSD driver is not a translation of GPL code.
- **Q4. Userland for M4.** FFmpeg's v4l2-request hwaccel: use upstream if it
  has landed in the version in ports, otherwise carry the patch set.
- **Q5. Names.** `media(4)`, `vdev`, `vbq` are working names.
- **Q6. Upstream path.** Phabricator reviews per component, starting with P1
  (dma-buf in base), which is useful on its own.

## M3 result (2026-10-05): media controller, request API, compound controls

Verified on the board (v4l2-compliance 1.23, guest build): vtest 53/53 and vconv
53/53 with `-s` (vconv takes requests, so "test Requests" runs the full path);
both `/dev/mediaN` nodes 8/8 with `-M`. `tests/abi` now also compares every
object-like MEDIA_* constant and every struct media_* size against Linux's
media.h (87 constants, 11 structs, 118 ioctls): our `uapi/linux/media.h` is
written from the documented interface, BSD-2-Clause, resolving Q1 for media.h.

Design points worth keeping:
- A request is a file of its own type (DTYPE 101 local). It holds one reference
  per bound buffer and the request's list holds one on each buffer, so a queued
  request outlives its descriptor and a buffer outlives its queue.
- Locks: `vreq_lock` (global sx) orders binding, QUEUE, REINIT, close and a
  queue letting go of its buffers; ctrl ioctls take it shared. The completion
  path (`vbq_buf_done`) and a driver's job never take it, so `stop_streaming()`
  may wait for a job while a queue holds it. Order: vreq_lock -> h->lock ->
  req->clk; req->mtx is a leaf below q->mtx.
- A request snapshots every control at `vctrl_request_setup()` (called from the
  driver's job, sleepable), not at completion: completion can run in a callout.
- Compound/array controls carry a payload; ENOSPC fills in the right size.
- FreeBSD has no EBADR; `EBADR` maps to EBADRPC in media.h. Unsupported
  requests on G_EXT_CTRLS give EINVAL (1.23 accepts nothing else).
- v4l-utils finds media devices through Linux sysfs; `tools/mi-sysfs-shim.c`
  (LD_PRELOAD) answers the two lookups it needs (directory listing and uevent).
- A panic found on the way: per-file control handlers had no `vdev`; fixed.

Not done: validation hooks for structured compound controls (arrive with the
first user, Cedrus), `vbq` request validate callback, subdev API.
