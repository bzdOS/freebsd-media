# Where the test streams come from

Written 2026-10-08 from the scripts in this repository; the owner confirms the items marked CONFIRM.

No third-party footage is used. Every stream and clip the repository, or its scripts, refer to is generated from a
synthetic source of FFmpeg's `lavfi` input by an encoder that ran on the host:

| Material | Source | Encoder | Where the command is |
|---|---|---|---|
| `flat.m2v`, `grad.m2v`, `i64.m2v` (M007) | `lavfi` test pattern, 64x64 (flat grey, gradient) | FFmpeg `mpeg2video`, all-I | `vectors/ve_ilib.py` `mkvec` (`src=`, `-q:v 3 -g 1 -bf 0`) |
| P/B MPEG-2 streams (M012) | `testsrc2`, 100x60 and 304x200, 9 frames | FFmpeg `mpeg2video` | `vectors/ve_ilib.py` (`-q:v 5`, options per case) |
| `play.m2v` (M038, 60 pictures, 352x288, I/P) | `lavfi` source, same generator as above. CONFIRM: the exact command is not recorded | FFmpeg `mpeg2video` | CONFIRM |
| H.264 streams `f1 f2 f3 wp wb smb cabac_i b2 b3 bt cqm cip db hd640 hd720 hd1080 t640 ...` (M014-M021) | `testsrc2` (macroblock-aligned sizes 64x64 to 1920x1088), fades for weighted prediction | libx264 with per-stream options (`weightp=2`, `bframes=2`, `b-pyramid`, `cabac=0/1`, 8x8 transform, scaling matrices, ...) | the options are stated in each M0xx note, e.g. M015 for `wb.264`; the front-end traces under `linux-work/h264-traces` (not in git) |
| Format and feature probes (M028-M035) | `testsrc2` 176x144 / 352x288 with `format=` / `fade` | libx264, JM reference encoder for SP/SI/FMO (M032-M035: `jm-*` streams) | the M0xx READMEs |
| Speed streams (M040) | `mandelbrot` 720x480, 1280x720, 1920x1088 | libx264 | `sys/dev/media/tools/ffmpeg/gen-streams.sh` |
| Player clips (M042) | `testsrc2` with burnt-in frame counter and a sine beep | libx264 / `mpeg2video`, AAC / MP2 | `sys/dev/media/tools/ffmpeg/gen-clips.sh` |

The conformance corpus (JVT) is not in this repository: it lives outside it, is downloaded from the public suite, and
only its names and results appear in the notes. Five short excerpts of it were removed from the tree on 2026-10-08.
