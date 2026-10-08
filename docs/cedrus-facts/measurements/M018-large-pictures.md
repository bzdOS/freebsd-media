# M018: large pictures decode, and the auxiliary allocation ladder (2026-10-07)

Method: the H.264 decoder test on the board (`tools/run-h264-test.sh`), streams
`hd720.264` (1280x720) and `hd640.264` (640x368) with their ffmpeg references, plus the
driver's own counters (`rejects`, `errors`) and the auxiliary-window sysctls.

- F:M018-a  **The driver decodes 1280x720 and 640x368 bit-exactly.** hd720, two
  pictures: `Y 921600/921600  C 460800/460800` each; hd640, two pictures:
  `Y 235520/235520  C 117760/117760` each. These are the first large-picture results for
  this driver; before the fix below, hd720 was rejected outright.
- F:M018-b  **The rejection was the auxiliary allocation, not the engine.** At
  `dev.cedrus.0.aux_mb` = 64 and 256 both hd720 pictures came back flagged ERROR with
  `slice rejected: 12` (ENOMEM) and no engine error at all; at 512 and 1024 both decoded
  bit-exactly. At 1280x720 the two scratch buffers are 3.5 MiB each (F:M017-d), and a
  256 MiB window could not place two contiguous runs of that size.
- F:M018-c  **The fix** (`fbsd-media`, this measurement): the progressive-window ladder
  in `cedrus_mem_alloc` now goes to at least 1 GiB — `MAX(cedrus_dma_mb, 1024 MiB)` —
  instead of stopping at the window the data buffers themselves use. With it, hd720 and
  hd640 decode at the **default** `aux_mb` (64), and the 176x144 control (f3, three
  pictures) still decodes 3/3. The engine addresses the whole of DRAM, so the wider
  bound is safe; the ladder still starts at `aux_mb`.
- F:M018-d  **What this does and does not say about M017-d.** The scratch size formula,
  `MAX(128 KiB, wmb*hmb*1024)` (3.5 MiB at 1280x720), remains an unverified guess in the
  sense that no measurement says a *smaller* buffer would fail — but it is now
  *sufficient*: the engine decoded 720p correctly with it, and the per-picture MV
  allocation (118784 bytes at 720p, F:M017-b) held as well. The 128 KiB floor is still
  the only measured scratch figure (M014-k, 64x64 to 176x144).

Open from here: the sizes could be tightened with a bisection on the scratch buffer
(shrink `wmb*hmb*1024` until the picture breaks), and the same test should be run on the
streams that are still untested at 720p-class sizes (`s320` is 11335 bytes, `q10` 15421 —
both 176x144-class though). Nothing here changes M016: the intermittent failure is at
176x144 and is not an allocation problem (F:M016-b's `aux_mb` row).

Artifacts: this file, the driver's `rejects`/`errors` counters, and the streams above
with their references in `linux-work/initramfs-h264/streams/`.
