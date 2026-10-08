# M042: a real player (fbplay) on the engine, 2026-10-08

`sys/dev/media/tools/ffmpeg/fbplay.c` turns ffkms into a player: FFmpeg demuxes (mp4, mkv, MPEG-TS, MPEG-PS, avi, raw
`.264`/`.m2v`, and audio-only mp3/aac/wav/flac/ogg), H.264 and MPEG-2 are decoded on the video engine through the
v4l2-request hwaccel and shown without a copy (same EGLImage/gbm/bzkms path as ffkms), the audio (AAC, MP3, MP2, AC-3,
FLAC, Vorbis, PCM) is decoded by libavcodec, converted by libswresample and played through OSS `/dev/dsp0`. The audio is
the master clock. Build: `build-ffmpeg.sh configure make player push` (the configure line gained the audio
decoders/parsers/demuxers and swresample; `player` cross-links fbplay and ffkms statically against `$W/install`). Test
material: `gen-clips.sh` (60 s, 30 fps, testsrc2 + burned-in frame number/time, a 1 kHz beep of 0.1 s at the start of every
second). Checks: `check-hashes.py` (frame md5 against the host's software decode), `cmp-nv12.py` (raw dump against it).
Board: A64, 4 x A53 @ 1152 MHz, guest FreeBSD 15.1, screen = 1120x276 @ 59 Hz (refresh measured 16.95 ms).

Design: demux thread -> bounded packet queues -> video decode thread (frames queue of 2) and audio thread (decode,
resample to S16 stereo, blocking `write()` = real-time pacing; after each write the clock is "end pts of the written
samples minus `SNDCTL_DSP_GETODELAY`", extrapolated with CLOCK_MONOTONIC in between). The main thread (owner of the
EGL context) learns the vblank grid from the page-flip events, picks the vblank nearest to the frame's due time, starts
the draw `render time + 4 ms` before it and sleeps with `clock_nanosleep` (no spinning); a frame whose vblank is already
more than half a refresh past, while a later frame is ready, is dropped. The first picture opens a gate and the audio
starts with it. Loops (`-l n`, `-L`) continue the timeline with an offset; SIGUSR1 seeks to the start. EOF: both
decoders are drained, the audio device is drained (`SNDCTL_DSP_SYNC`), the summary is printed.

## Facts

- F:M042-a  **Audio device runs 3.84% slow (not a player bug).** `/dev/dsp0` (pcm0, simple-audio-card) accepts 48000 Hz
  s16 stereo but consumes 46157 samples/s: a test that wrote 21.33 s of audio needed 23.24 s wall; the ratio
  (written minus GETODELAY)/wall is 0.9616 and steady from the first second. `dmesg` repeats `pcm0: failed to set
  sysclk for codec node` / `... for aux node` on every open, which is the likely cause (codec/I2S sysclk not set, so the
  DAI runs from a wrong clock). Consequence: with an audio-master clock the video plays 4% slow and the pitch is 4% low.
  `fbplay -k 0.9616` treats the device as a 46157 Hz sink (swresample converts to that rate), which makes the pitch and the
  clock right: 60 s of content take 59.96..60.03 s wall in every run below. Without `-k` a 5 s clip took 5.29 s (5.8% slow,
  `nok.log`). Not verified: audible output (no speaker, no capture path). What is verified is the sample consumption
  rate and the GETODELAY-based clock. The null sink (`-A null`) runs the same clock on a real-time paced dummy device and
  needs no `-k` (5.00 s clip: 5.10 s wall, 0 underruns, includes start-up).
- F:M042-b  **Full-length plays, 60 s clips, 1800 frames each, audio on `/dev/dsp0` with `-k 0.9616`**
  (one run each; `PLAYED` wall time, drops = frames skipped because a later one was already due, "late" = more than
  1.25 refresh behind the clock at the flip event, "missed" = the targeted vblank was missed by > half a refresh):

  | clip | decoded | presented | dropped | late | missed | flip error vs clock, mean / sd / min..max (ms) | CPU, % of one core | audio underruns |
  |---|---|---|---|---|---|---|---|---|
  | c360.mp4 H.264 640x360 + AAC | 1800 | 1795 | 5 | 1 | 5 | -2.3 / 2.5 / -7.6..+22 | 12 | 0 |
  | c720.mp4 H.264 1280x720 + AAC | 1800 | 1799 | 1 | 3 | 3 | +1.0 / 2.4 / -3.2..+51 | 13 | 0 |
  | c1080.mp4 H.264 1920x1080 + AAC | 1800 | 1800 | 0 | 1 | 1 | +1.2 / 6.7 / -10..+24 | 16 | 0 |
  | c360.mkv H.264 + MP3 | 1800 | 1799 | 1 | 0 | 1 | -4.7 / 2.0 / -10..+11 | 13 | 1 |
  | m360.ts MPEG-2 640x360 + MP2 | 1800 | 1800 | 0 | 0 | 0 | +0.7 / 1.7 / -3.3..+4.6 | 14 | 0 |
  | m720.ts MPEG-2 1280x720 + MP2 | 1800 | 1800 | 0 | 1 | 0 | +1.3 / 3.6 / -4.3..+24 | 13 | 0 |
  | m1080.ts MPEG-2 1920x1080 + MP2 | 1800 | 1800 | 0 | 1 | 1 | +4.8 / 1.7 / +0.5..+25 | 14 | 0 |
  | c360.264 raw H.264 (`-r 30 -N`, no audio, free-running clock) | 1800 | 1799 | 1 | 2 | 2 | +4.8 / 1.7 / +1.7..+47 | 6 | - |
  | m360.m2v raw MPEG-2 (`-r 30 -N`) | 1800 | 1800 | 0 | 0 | 2 | -2.4 / 1.6 / -5.3..+19 | 6 | - |

  Totals: 16200 frames decoded, 16192 presented, 8 dropped (0.05%). The flip error is bounded by half a refresh (+-8.5 ms)
  by construction (30 fps content on a 59 Hz screen: every frame waits for the nearest vblank); the outliers of +20..+50 ms
  are single frames whose vblank was missed (a scheduling or GPU delay of a refresh or more). The 1080p sd of 6.7 ms is the
  same uniform +-8.5 ms spread (the phase of the vblank grid does not lock onto the frame cadence in that run), not jitter.
- F:M042-c  **A/V drift over 60 s.** Once a second the player logs the audio clock minus the pts of the picture on the
  screen (`clk-vpts`): c360 +0.016..+0.035 s (mean +0.027), c720 +0.026..+0.051, c1080 +0.002..+0.021, c360.mkv
  +0.022..+0.025, m360 +0.026..+0.031, m720 +0.027..+0.034, m1080 +0.032..+0.035 (59-60 samples each). The value is the
  time since the last flip (up to one 33 ms frame period), it has no trend over the 60 s: the video neither runs away
  from nor falls behind the audio. Content time against wall time: 59.96..60.03 s for 60 s. Not measured: an independent
  (external) measurement of lip-sync; the clock is the player's own, and the audio path has the device's output latency
  accounted by GETODELAY (largest 44.4 ms) but its real analogue latency is unknown.
- F:M042-d  **Frames submitted equal the software decode (H.264).** `-H` hashes (MD5 of the cropped NV12 picture read back
  from the decoder's dma-buf just before the draw call) against `ffmpeg -pix_fmt nv12 -f framemd5` on the host: c360.mp4
  1799 of 1799 presented frames identical (the 1800th was the dropped one); c720 592 of 592; c1080 289 of 289 (these two
  limited to 20 s, the read-back costs CPU and at 1080p roughly half of the frames are dropped in that mode); the 5 s
  clip played three times with `-l 2`: the 150 frames of the first pass identical, the later passes carry offset pts.
  **MPEG-2** is not bit exact against FFmpeg's software IDCT, as known (M038): of 240 presented frames (`-D` dump,
  8 s of m360.ts) every frame differs somewhere, 0.45% of the luma samples and 0.70% of the chroma samples differ, by 1
  in 99.8% of those cases; maximum difference 3 (two luma and two chroma samples out of 83 million). The picture on
  the screen after the GPU's YUV->RGB conversion and scaling was not compared (no readback of a scanout buffer, M040-c).
- F:M042-e  **EOF, loop, seek.** At EOF both decoders are drained (all 1800 of 1800 frames come out, the last B-pictures
  included), the audio device is drained, the summary is printed and the process exits.
  `-l 2` on a 5 s clip: three passes, 456 frames decoded, 452 presented, 4 dropped, timeline continuous (pts 5.1, 10.2,
  ...), 0 audio underruns; each pass boundary costs one 100 ms interval between flips (the loop offset is the end of
  the longer stream, the AAC track ends 0.1 s after the video, so the last picture is held that long). A SIGUSR1 at 9 s
  into a play restarted the stream from its start (the decoders flushed, the queues purged, the audio device reset,
  timeline offset = current clock): 4 frames dropped, 1 audio underrun at the seek, playing on to the 15 s limit; that the
  first picture after the seek is frame 0 was not checked frame by frame.
- F:M042-f  **Cost.** 12-16% of one core with AAC/MP3/MP2 decode, resampling and the GPU draw (6% video only), 8-9.4 s of CPU
  for 60 s of 640x360..1920x1080 content; 8 EGL images (the capture buffers of the pool) at every size; the 3 MB
  contiguous 1080p buffers were allocated without failure after a fresh boot (M040-d still applies to a worn guest).

## Defects and traps found (nothing patched in sys/dev/cedrus or sys/dev/media)

1. **Audio rate / sysclk** (F:M042-a): not in the media stack; belongs to the pcm0 simple-audio-card (codec sysclk). Until fixed
   a player has to calibrate (`-k`); the 0.9616 is specific to this board's current DTB/clock setup.
2. **Occasional backward step of the audio clock** at an update: 67.8 ms (c360.mp4) and 54.1 ms (m720.ts) in two of ten
   full runs, <= 1.3 ms in the others. It coincides with outliers of the flip error; the cause (a GETODELAY value that
   is stale for one write, or a descheduled audio thread) was not found.
3. **`exit()` hangs** in the process teardown (all threads gone, one thread in `_umtx_op`, after the summary): seen at the
   end of every play before `_exit()` was used; the hang is in a library's exit handler of the GL/gbm stack (not
   isolated further). fbplay ends with `fflush; _exit(0)`.
4. Hwaccel initialisation fails with `Failed setup for format drm_prime` and FFmpeg falls back to broken software
   frames when `cedrus` is not loaded (`/dev/video0` missing), e.g. while another agent reloads the modules; a player
   must treat a non-DRM_PRIME frame as fatal (fbplay does).
5. The packet queues hold 256 video and 512 audio packets; a file whose audio is stored far from its video (non-interleaved
   mp4) would deadlock the demuxer against the full queue. Not met by any test file.

## Reproduce

`gen-clips.sh` (host), `build-ffmpeg.sh configure make player push`, push the clips to `/var/tmp/clips`, KMS module order of
M040, then on the guest `LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib:/usr/local/lib /var/tmp/ffv4l2/bin/fbplay -k 0.9616 clips/c720.mp4`.
Options: `-s sec -l n -L -n -r fps -A null -a dev -N -q n -H file -D file -v -k x`.

## Re-check on merged main (blocks A, B, D and generated media.h), 2026-10-08

Fresh guest, `board-bootstrap.sh`, then the M040 module order (bridged dmabuf, drm, lima, bzkms):
`fbplay -k 0.9616 c720.mp4` played 58.8 s, decoded 1768, presented 1759, dropped 4, flips refused 0, audio underruns 0,
CPU 13% of one core. `dev.cedrus.0`: errors 0, timeouts 0, pool_allocs 642, pool_fails 0, pool_live 0 after the run, so the
screen path works with the reserved pool. `all.sh 2 idle` 16/16 and `ffmpeg-test.sh f3 smb wb t640 hd640` 0 differing samples.
Note: `board-bootstrap.sh` does not push `all.sh`, `assay.sh`, `rate.sh`, `cmpyuv.c`; scp them to /tmp/ve by hand.
