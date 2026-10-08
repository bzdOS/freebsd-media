# M039: FFmpeg decodes through the driver (V4L2 Request hwaccel), 2026-10-08

Consumer test of the stateless V4L2 stack: an unmodified FFmpeg userland (Kwiboo's `v4l2-request-n8.1` branch; the
FFmpeg releases up to 9.0.2 have no stateless V4L2 hwaccel) decodes H.264 with `-hwaccel v4l2request` on the board
through `/dev/video0` + `/dev/media0`. Built on the build host (cross, `tools/ffmpeg/xconfigure.sh` +
`libudev-shim.c`), copied to `/var/tmp/ffv4l2` on the guest; building on the board crashed `gmake` and was
abandoned (the guest has ~850 MB).

- F:M039-a  **39 of 39 streams exact** (the committed streams, I/P/B, CAVLC/CABAC, 8x8, scaling lists, weights,
  8 refs, multi-slice, 64x64 to 1920x1088; `tools/ffmpeg/ffmpeg-test.sh`), no engine errors, rejects or timeouts.
  The output is FFmpeg's normal software readback (`-hwaccel` without `-hwaccel_output_format` copies the DRM_PRIME
  frame out through `transfer_data_from`, which maps the dma-buf).
- F:M039-b  What FFmpeg needed that the driver did not have:
  1. **`DMA_BUF_IOCTL_SYNC`** on our dma-buf file (`sys/dev/dmabuf`): FFmpeg brackets CPU access with it. The
     buffers are uncached and coherent, so the ioctl only validates its flags. A `linux/dma-buf.h` with the
     published structure and number is now in `sys/dev/media/uapi`.
  2. **`header_bit_size` in FFmpeg's convention.** FFmpeg sends the bits from the NAL header byte to `slice_data()`
     counted in the RBSP, without the start code and without emulation prevention bytes (39 for a 31-bit header
     in `f3`). Our front end had given the bits from the first byte of the data, start code included. The engine
     needs the start code skipped too, so the driver adds 24 or 32 bits (`dev.cedrus.0.h264_hdrmode`, default 1);
     the front end now emits FFmpeg's convention by default (`h264ctl.h` `hdr_abs` = 0) and the Linux register rig
     keeps the old one (`hdr_abs` = 1). EPBs are not skipped bits in either (M026-f).
  3. **A table index per picture that stays the same while the picture is a reference.** FFmpeg numbers its DPB
     entries by recency, so a picture's index changed on every picture. The engine files the table indices of a
     picture's references in its motion vector buffer and reads them back when the picture is the co-located one of
     a later B picture; with changing indices temporal direct prediction broke in a few macroblocks of two B
     pictures of `f3` (378 and 102 samples), nothing else. The driver now keeps a table slot per capture buffer for
     as long as the buffer is in the DPB (`tslot`/`tmap` in `cedrus_h264.c`) and maps the lists through it. Our own
     front end's stable indices map to themselves.
- F:M039-c  Traps: `-hwaccel v4l2request -vf hwdownload` fails (the frames are already system frames at that
  point); the udev lookup FFmpeg does is replaced by a 150-line shim over devfs (`libudev-shim.c`); `configure` must
  be re-run after adding `linux/dma-buf.h`, or `hwcontext_drm` returns ENOSYS for the mapping.

Not done here: MPEG-2 and field/MBAFF through FFmpeg, speed, GStreamer.
