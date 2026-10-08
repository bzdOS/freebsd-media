# M021: the whole stream set, and the scratch formula reduced (2026-10-07)

Method: the H.264 test over every stream committed with its ffmpeg reference
(`tools/board-bootstrap.sh` puts them on the board; `linux-work/h264-traces/sweep.sh`
runs them), three pictures each, then the same streams again after the driver's scratch
allocation was reduced.

- F:M021-a  **The whole set decodes.** 25 streams — `s64 s192 s320 q10 q40 ref8 highp
  cqmp cip b3 db bt sl3 nodbf wp high_i high_cavlc cabac_i p_cabac cqm cqpo b2
  b2_cavlc p1 p3` — all come back PASS on every picture the stream has, i.e. intra, P and
  B slices, CAVLC and CABAC, 8x8 transform, scaling matrices, weighted prediction, eight
  references, sizes from 64x64 to 320-wide, deblocking off. Two pictures out of about
  sixty-five came back with the buffer flagged ERROR (`sl3`, `p1`) — the M016 engine
  error, at the rate it has there — and **no request was left waiting anywhere**, which
  is the M020 fix holding on 15-slice pictures as well.
- F:M021-b  **The scratch formula is reduced from `wmb*hmb*1024` to `wmb*hmb*256`**, on
  M019's measurement (64 KiB sufficed at 640x368 with P and B, 128 KiB at 1280x720). The
  new formula asks for 115 KiB at 640x368 and 450 KiB at 1280x720 — still above every
  measurement, and small enough that two contiguous runs are easy to place: **hd720 now
  decodes with `aux_mb=128`**, where the 3.5 MiB version needed at least 512 MiB
  (F:M018-b). The per-picture MV formula is left alone: it is only about 2x the measured
  need and its absolute size is small (116 KiB at 1280x720).
- F:M021-c  **Re-validated after the reduction:** hd720 (1280x720), hd640 (640x368),
  t640 (640x368 with P, B and B-pyramid), f3, wb and smb all still decode, and the
  25-stream sweep was not re-run in full — the shapes above are the ones with the largest
  scratch need, which is what the change touches.

- F:M021-d  **1080p (2026-10-07).** `hd1080` — 1920x1088, 6 pictures I P B P B P, three
  references, CABAC, 8x8 transform (x264 `testsrc2`, macroblock-aligned because the test
  compares the whole coded picture) — decodes exact on every picture, 6 of 6 runs, no
  errors. The scratch formula (`wmb*hmb*256`, 2 MB-ish at this size) is therefore
  validated at 1080p. Limit seen with it: 20 capture buffers of 3.1 MB each need that much
  contiguous memory, which a fragmented guest does not have (`REQBUFS ... ENOMEM`); the
  test now asks for fewer when that happens.

- F:M021-e  **Explicit weights (2026-10-07).** Three fade streams (x264 `weightp=2`,
  176x144, 12 pictures: `wf1` one reference, `wf3` three, `wfb` three with B pictures and
  `weightb=1`, which x264 turns into implicit bipred weights) carry explicit P weights
  with luma/chroma denominators 0 to 6, non-default weights in 60+ of 64 entries and
  non-zero offsets in 6 to 10 pictures. All 36 pictures decode exact. Explicit weights
  in B slices (`weighted_bipred_idc` 1) still have no stream: no encoder at hand writes
  them. The streams are in `linux-work/initramfs-h264/streams` with their references.

Open: the reduction is validated on the shapes that matter for it, not on the whole set
at once; a stream with a much larger picture (1080p) would be the next thing to try, and
none is committed.

Artifacts: this file; the sweep script and `tools/board-bootstrap.sh`; the streams.
