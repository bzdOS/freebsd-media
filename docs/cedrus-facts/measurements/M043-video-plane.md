# M043: the decoder's picture on the display engine's video plane, 2026-10-09

The hypervisor half is "scanout v3" (microkernel/docs/zero-copy-scanout.md section 10): the HDMI mixer's VI channel as a
guest plane, NV12/NV21/YUYV/XRGB8888, scaled by the VSU, converted by the channel's CCSC, drawn over everything, staged
by `SCANOUT_R_VID_*` (0x0A002000 + 0xA0..0xC8) and applied at the next vblank by a write of `VID_CTRL`. The guest half
is new: `hal/bzkms` (bzdOS/bsdOS, branch `de2-plane`) has an NV12 **overlay plane**; `tests/cedrus_plane.c` (MPEG-2,
through V4L2 directly) and `tools/ffmpeg/ffplane.c` (H.264/MPEG-2 through FFmpeg's v4l2-request hwaccel) put decoded
frames on it. No GPU: no lima, no render node (`/dev/dri` has `card0` only), no copy, no EGL.

Path of a frame: capture buffer (reserved pool, physically contiguous, uncacheable) -> `VIDIOC_EXPBUF` -> dma-buf fd ->
`drmPrimeFDToHandle` on bzkms -> `drmModeAddFB2(NV12, pitch, chroma at +pitch*height)` (once per buffer) -> one atomic
commit per frame (FB_ID, CRTC_ID, SRC_*, CRTC_*, COLOR_ENCODING; `NONBLOCK | PAGE_FLIP_EVENT`) -> bzkms writes the
registers and `VID_CTRL` -> EL2 latches at the vblank -> the CRTC's flip event of that commit.

## Facts

- F:M043-a  **The plane.** bzkms registers `DRM_PLANE_TYPE_OVERLAY` plane 34 (formats NV12, NV21, YUYV, XRGB8888,
  COLOR_ENCODING BT.601/BT.709, limited range). The destination is in SCREEN coordinates (1920x1080 here, read from
  the scanout device), not in the 1120x276 guest window's. The driver refuses in `atomic_check` what EL2 would refuse
  (fractional source, odd 4:2:0 geometry, source wider than 2048, downscale beyond 2x, unaligned or >4 GiB plane address,
  destination off the screen) so the ioctl fails instead of the commit being silently ignored; a refusal that EL2 still
  makes is noticed through `VID_REJECT` and logged (first 8, then 1 in 1024). Framebuffer limits are 16..4096.
  The source origin is honoured by moving the plane addresses (the hardware has no source offset).
- F:M043-b  **A native dmabuf is imported by its physical segments.** The bridge in `dmabuf.ko` has no `vmap`, and its
  `map_dma_buf` wants a DMA-capable device bzkms does not have, so `drmPrimeFDToHandle` of a capture buffer failed with
  EOPNOTSUPP. bzkms now asks the native core (`dmabuf_sglist`, module `dmabuf_base`) when `exp_name` is `dmabuf_base`;
  one contiguous run is required as before. lima-exported buffers still go through `vmap`.
- F:M043-c  **Register level, MPEG-2** (`cedrus_plane`, play.m2v 352x288 -> 1320x1080 at 300,0, 60 s, flips waited for):
  3601 frames, 3601 flip events, `VID_COUNT` advanced by exactly 3601, `VID_REJECT` 0, after every flip the staged
  ADDR0/ADDR1/PITCH0/PITCH1/SRC_SIZE/DST_COORD/DST_SIZE/FORMAT/CTRL equal that frame's (0 mismatches), 3599 of 3601 flips
  exactly one vblank after the previous one (1 skipped; a second 60 s run: 4 skipped of 3599). Luma and chroma addresses of
  all 8 capture buffers were checked independently: a marker word written through the user mapping of each buffer was read
  back with `dev.cedrus.0.peek` at the address EL2 was given (8 of 8 match).
- F:M043-d  **Register level, H.264** (`ffplane`, c720.mp4 1280x720, 100 s, whole screen 1920x1080, BT.709): 6002 frames shown,
  6002 vblanks, `VID_COUNT` +6002, `VID_REJECT` 0, 0 register mismatches, every flip exactly 1 vblank after the previous one
  (0 skipped), all 7 buffers inside the engine's pool (`0x69000000..0x6d000000`), `dev.cedrus.0` errors 0 timeouts 0.
- F:M043-e  **The DE2 mixer itself, read from EL2** (`hv.read_words` over the debug channel, while `ffplane` played 720p to
  1920x1080): VI0 `ATTR 0x801` (enabled, NV12), `SIZE 0x02cf04ff` (1280x720), `PITCH0/1 0x500`, VSU `CTRL 1`, `OUTSIZE
  0x0437077f` (1920x1080), `YINSIZE 0x02cf04ff`, `CINSIZE 0x0167027f` (640x360), blender pipe 1 `INSIZE 0x0437077f`.
  90 samples of `LADDR0/LADDR1` in 6 s gave exactly the 7 (luma, chroma) pairs of `ffplane`'s buffers (e.g. `0x69cc6000 /
  0x69da7000`), each pair a buffer's own luma and chroma, all 7 in rotation. So the hardware fetches the decoder's buffers
  and the registers are not merely the shadow EL2 keeps for the guest.
- F:M043-f  **Rate and cost** (60 Hz is the panel's: EL2 counts 60.0 vblanks/s): the plane is paced by the flip events, so the
  frame rate is the refresh rate, 59.9..60.0 fps in every run below. Process CPU: `ffplane` H.264 720p -> 1080p
  4.5% of one core (100 s), 1080p -> window area 5.0%, 1080p -> full screen 6.5%; `cedrus_plane` MPEG-2 2.3% (the harness
  memsets every capture buffer with 0xa5 each picture through the uncached mapping, which is most of it). The machine was
  90..96% idle; with `ffkms` (the GPU path, M040-b) the same 720p stream needs 10% of one core and lima.
- F:M043-g  **No tearing by construction, and checked.** Every flip is one staged update applied at a vblank by EL2. A
  capture buffer is not handed back to the decoder while it is on screen or staged, nor for one vblank after the flip that
  replaced it (`plane_hold` in `cedrus_plane`; the frame reference in `ffplane`). With `CEDRUS_PLANE_SUMS=1` a checksum of
  every picture taken when it is staged and again when it is released: 1201 of 1201 unchanged (0 buffers were written
  while held; that run costs 86% of a core, the buffers are read through an uncached mapping), and the decoder was never
  given a held buffer (0 times).
- F:M043-h  **Scaling limits met in practice.** The hypervisor refuses a downscale beyond 2x, so the 1120x276 guest-window
  area cannot hold a 720p or 1080p picture (720/276 = 2.6): both tools enlarge such a destination to half the source
  (720p -> 1120x360 at 16,24; 1080p -> 1120x540 at 16,0) instead of failing. An off-screen destination
  (`-a 1800,1000,300,300`) is refused by bzkms's check (EINVAL); with the check removed it was refused by EL2
  (`VID_REJECT` 181 of 181, the plane stayed as it was) and logged.
- F:M043-i  **Not an effect of the plane: `ffplane`'s first long run hit EOF at 30 s** (c720.mp4 is 1800 frames; at 60 fps it
  is over in 30 s), a byte seek does not restart an mp4, and the loop spun (140 threads, 99% CPU) until the run ended. Fixed
  (timestamp seek first, byte seek for raw streams, give up after four empty passes).
- F:M043-j  **The vblank stalls were the hypervisor's SD/eMMC request queue, not the plane.** Reproduced with `cedrus_plane`
  (`CEDRUS_PLANE_TRACE=1`): a flip event arriving 2..7 vblanks after its commit (40..260 ms), only while the guest was doing
  disk I/O. The commit was on time (`VID_COUNT` advanced with it), EL2's vblank count kept ticking, and a sampler thread on
  another CPU saw both registers move without a gap, so neither the test's wait loop nor EL2's apply at the vblank was late;
  the guest CPU that should have delivered the event was away. Controlled load, 30 s runs, 1800 flips: raw reads of
  the SD card with `dd bs=16k/64k/256k` 0 vblanks skipped, `bs=1m` 66..135 skipped (events every 54 ms for the length of the
  read, max flip interval 68..258 ms); 16 MiB writes + `sync` 6..10 skipped; raw read of the eMMC (root disk) `bs=1m`
  stalls up to 183 ms (7 vblanks). Cause (counters read from EL2, `hvdbg`): the guest queues about 17 requests of 60 KiB
  for one MiB and the IDMAC queue of `vblk_sd.c`/`vblk_emmc.c` had 16 slots; the request that found it full was not queued
  but served in the doorbell trap after draining the whole queue there (`g_sdq_sync_waiters` non-zero during the read,
  `g_sd_dma_ok` +578 against `g_sd_idma_posts` +546 for 32 MiB, i.e. one synchronous serve per MiB), so one vCPU sat in EL2
  with interrupts masked for 16 transfers (16 x 3.4 ms = 54 ms on the SD card, 90..180 ms on the eMMC), and its timer ticks,
  callouts and wakeups waited with it. A queued request takes at least three ring descriptors and the ring has 256, so the
  queue is now `QUEUE_MAX / 3 + 2` slots and cannot overflow (microkernel branch `vblank-sdq`). After the change, with the
  guest reloaded: SD `bs=1m` reads 0 skipped, max flip interval 20.5 ms; eMMC `bs=1m` reads 0 skipped; 60 s of SD reads +
  eMMC reads + 24 MiB writes + `sync` together 0 skipped (3601 flips, max 21.5 ms); 240 s idle 0 skipped; queue depth
  reached 32 (twice the old limit), every request completed through the queue (`g_sd_dma_ok` = posts = done), disk
  throughput unchanged (SD 18.4..19.5 MB/s, eMMC 10.8..11 MB/s), md5 of concurrent raw reads identical. A change of
  `bzkms` to sample the vblank counter from a direct callout and deliver from a real-time thread did **not** help (66..87
  skipped) and was dropped: the delay is a vCPU held in EL2, not scheduling in the guest.

## Not verified

- What the panel shows: the owner watched the screen on 2026-10-09 and reported that the picture was there: first in a window, then
  full screen. Which stream or tool was running, and whether colours (the BT.601/BT.709 choice), orientation, sharpness and tearing were looked at one by
  one, was not recorded. Beyond that report, only the registers, the mixer's fetch addresses and geometry (F:M043-e) and the
  buffer integrity (F:M043-g) are checked.
- The decoded picture was not compared with a reference after the VSU and the CSC (nothing reads the panel back).
- 1080p and larger sources were run only with H.264 (c1080.mp4, 899 of 900 flips in one vblank each, one 57 ms stall at the
  start of one run); the MPEG-2 harness skipped a few vblanks per minute (4 of 3599, 1 of 3601, 21 of 7193 in a 120 s run
  with 3 stalls up to 184 ms) until F:M043-j, `ffplane` none in 100 s. The cause of those stalls is F:M043-j.
- The UI overlay planes of scanout v2 (UI1 layers 2 and 3) are not exposed by bzkms; YUYV and XRGB8888 on the video plane
  and NV21 are in the plane's format list but were not run.
- `VID_CTRL` write ordering against another CPU's flip (two clients) is serialised only inside bzkms (a mutex).

## Reproduce

Host: bzkms in `bsdOS` branch `de2-plane`, `hal/bzkms` (Makefile comment has the cross-build line, against
`/opt/bzdos/drm-kmod`); `tools/board-bootstrap.sh f1`; guest module order as in M040 but WITHOUT lima: `kldunload bzfb lima`,
`kldload` the bridged `dmabuf.ko`, `drm`, `bzkms.ko` (from /tmp, md5 verified; reboot after swapping bzkms). Then
`cc -DCEDRUS_PLANE -DCEDRUS_KMS ... -o cedrus_plane cedrus_plane.c -ldrm` and
`CEDRUS_PLANE_SECONDS=60 ./cedrus_plane play.m2v play.m2v`, or `ffplane -s 60 [-a full|window|x,y,w,h] [-S] c720.mp4`
with `LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib:/usr/local/lib` (`build-ffmpeg.sh player` builds it).
