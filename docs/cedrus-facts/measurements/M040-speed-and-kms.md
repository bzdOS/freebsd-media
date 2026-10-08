# M040: decode speed and the picture on the screen without a copy, 2026-10-08

Method: FFmpeg (Kwiboo's v4l2-request branch, M039) on the board, three generated streams (x264, High/Main profile,
2 B-frames, 3 references, GOP 60, mandelbrot content: 720x480 300 frames, 1280x720 300 frames, 1920x1088 150
frames), measured with `ffmpeg -benchmark` (`tools/ffmpeg/speed-test.sh`) and with `tools/ffmpeg/ffkms.c`, a small
player on libavcodec + EGL/gbm/KMS. Board: A64, four Cortex-A53 at 1152 MHz, VE clock 297 MHz, 850 MB, FreeBSD 15.1
guest under the hypervisor. One run each; the numbers repeat within a few percent where a second run was made.

- F:M040-a  **Decode speed** (frames per second; CPU = user+system seconds of all cores for the whole stream):

  | stream | hardware, frames kept in dma-bufs | hardware, copied to system memory | software (libavcodec, 4 threads) |
  |---|---|---|---|
  | 720x480 | **424 fps**, CPU 0.34 s | 169 fps, CPU 2.1 s | 193 fps, CPU 5.8 s |
  | 1280x720 | **177 fps**, CPU 0.44 s | 67 fps, CPU 4.8 s | 81 fps, CPU 14.3 s |
  | 1920x1088 | **82 fps**, CPU 0.32 s | 30 fps, CPU 5.2 s | 36 fps, CPU 15.5 s |

  The engine and the driver (one request per slice, one slice per picture here) decode 1080p at 82 frames per
  second using under 0.4 s of CPU time for 150 pictures (about 2 ms a picture, all of it the driver and FFmpeg's
  bookkeeping). The software decoder needs all four cores to reach 36 fps. Copying the picture out and converting it
  (what `-hwaccel` does by default) costs more than the decode: with the copy the hardware path is no faster than
  software at 720x480 and only 1.2 times faster at 1080p, so a consumer has to take the frames as dma-bufs.
- F:M040-b  **To the screen without a copy** (`ffkms`): the decoder's dma-buf is imported as an NV12 EGLImage on lima,
  a textured quad converts and scales it into a gbm scanout buffer, the buffer is page-flipped on bzkms and every flip
  is waited for; the CPU never reads or writes a picture. The screen is the hypervisor's guest window, 1120x276 at 59 Hz.

  | stream | with page flips (vsync paced) | no flips (decode + GPU only) |
  |---|---|---|
  | 720x480 | 60.0 fps, 9% of one core | 412 fps, 57% of one core |
  | 1280x720 | 59.8 fps, 10% | 177 fps, 30% |
  | 1920x1088 | 57.6 fps, 11% | 82.5 fps, 16% |

  Every frame of the 8-second runs was decoded and shown (0 refused flips). 1080p is the first case that is not
  pinned at the refresh rate: 57.6 against 60 means a few frames in a second miss their vblank (the GPU's conversion
  and scaling of a 1080p picture into the small window plus the decode in one thread); the decode alone (82 fps)
  and the GPU together (no flips) hold 82 fps, so the margin is thin but real.
- F:M040-c  Not measured: the picture on the screen was not compared with a reference (a read-back of a scanout
  buffer hangs the GPU on this stack, and the earlier GPU import tests verified the YUV-to-RGB conversion pixel by
  pixel for MPEG-2); the H.264 decode itself is bit-exact on the streams M039 checked (first 30 packets of each of 173 corpus streams, M039-c). `ffkms` shows the frames in the order `avcodec_receive_frame` returns them, which is presentation order
  (FFmpeg reorders B pictures itself), but it has no clock, audio or container handling, so it is a throughput test, not a player.
- F:M040-d  Trap: 1080p needs 3 MB contiguous OUTPUT buffers; on a guest that has run many tests (and loaded the GPU
  stack) FFmpeg's `CREATE_BUFS` fails with ENOMEM (`Failed to create buffer of type 1`) and it falls back to software
  frames. A freshly rebooted guest does it. The module order for the screen: our `dmabuf_base`, `media`, `cedrus` first;
  then unload `bzfb lima drm dmabuf`, `kldload` the bridged `dmabuf.ko` (`fbsd-obj/drm-kmod-arm64/dmabuf`), then
  `drm lima bzkms`; run with `LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib:/usr/local/lib`.
