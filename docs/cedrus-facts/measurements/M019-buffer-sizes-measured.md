# M019: what the MV and scratch buffers really need (2026-10-07)

Method: the driver's two experiment knobs `dev.cedrus.0.mv_kb` and
`dev.cedrus.0.scratch_kb` force the per-picture MV buffer size and the scratch buffer
size instead of the formula, and the sizes are bisected downwards until the decode
breaks. Two streams: `wb.264` (176x144, half = 1584 bytes) and `t640.264` — 640x368,
`cabac=1 ref=3 bframes=2 b_pyramid=2 weightb=1 weightp=2`, 10 pictures, generated for
this measurement with ffmpeg + libx264, six pictures decoded per run.

- F:M019-a  **At 640x368 the per-picture MV buffer must be over 16 KiB.** A 16 KiB
  buffer fails **deterministically** — five runs in a row, each 2 PASS / 4 FAIL, the B
  pictures failing — while 17 KiB and 18 KiB pass (17 KiB showed the usual one flake in
  three runs). The half is 14720 bytes, so the requirement is about **1.15-1.2 x the
  half**, not the 2 x the formula assumes.
- F:M019-b  **At 176x144 a 2 KiB buffer is enough.** Eight runs of six pictures with a
  2048-byte buffer were all 6/6; 3072 bytes is fine as well (the one 4/6 run was the
  intermittent failure of M016 — five repeats gave 6/6). The half is 1584 bytes.
- F:M019-c  **The bottom MV half appears unused for frame pictures.** A 2 KiB buffer
  cannot hold the second half at +1584 *plus* its data, yet the decode is bit-exact; if
  the engine wrote the bottom half it would run past the buffer. This also explains
  F:M019-a: 16 KiB is only 1.1 x the *top* half, and it breaks, so the engine writes
  somewhat past a bare half — but nowhere near two of them.
- F:M019-d  **The scratch requirement is far below the formula.** 64 KiB suffices at
  640x368 (a stream with P and B pictures; the formula gives 920 KiB) and 128 KiB at
  1280x720 (I-only; the formula gives 3.5 MiB).
- F:M019-e  **The driver's formulas over-allocate by roughly 2x (MV) and 10x
  (scratch).** They are left as they are: over-provisioning cannot corrupt anything,
  the measured minimums come from two sizes only, and a smaller MV buffer is exactly the
  kind of change that needs more stream shapes behind it. The numbers are here for
  whoever wants to shrink them — which would also make the M018 allocation rejection less
  likely, since smaller contiguous runs are easier to place.
- F:M019-f  **The 640x368 stream with P, B and B-pyramid decodes bit-exactly**, six
  pictures of six (`Y 235520/235520  C 117760/117760` each): the first large P/B result
  for this driver, next to the I-only 1280x720 of M018.

Artifacts: this file; the knobs (`mv_kb`, `scratch_kb`) in `fbsd-media`; `t640.264` and
`t640.yuv` in `linux-work/initramfs-h264/streams/` (generated, not committed).
