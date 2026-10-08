# M016: H.264 on the board — the intermittent failure and what it is not (2026-10-06)

> **Superseded by M023 (2026-10-07):** the cause was the driver's wait after a header-skip command (it waited for a bit that is always set); the engine error is gone and the margin / request-pipeline readings below are wrong. The measurements stand; the conclusions do not.

Method: the decoder test (`fbsd-media/sys/dev/media/tests/cedrus_h264.c`, run through
`tools/run-h264-test.sh`) on the board, 50–100 decodes per configuration, counting runs
in which a picture failed; the driver's own counters (`rejects`, `errors`) and its
experiment knobs (`dev.cedrus.0.h264_*`) as instruments. The reference is ffmpeg's
decode of the same stream, picture by picture.

- F:M016-a  **The failure is real and intermittent, at 5–14% of runs.** `f3.264`
  (I, P, B; three pictures) fails in 7–9 runs out of 100 — 5–6 with a **partial**
  picture and no error flag, 2–4 with the capture buffer flagged ERROR. `wb.264` fails
  in 2–10 runs out of 100 depending on the run. `smb.264` (15 slices per picture) has
  not been seen to fail. `rejects` is 0 in every run: no slice is refused.
- F:M016-b  **Rejected by measurement** (no effect beyond sampling noise on `f3.264`,
  20 runs each): `h264_flush` (write back + invalidate the direct-map alias of the
  engine's buffers before every slice), `h264_post_delay` (2 ms after the finish
  interrupt), `h264_poll` (complete by polling instead of the interrupt),
  `h264_reset` (reset the engine before every slice), `h264_delay` (2 ms before
  programming a slice), `h264_zero=7` (zero the picture, the scratch and the MV
  buffers), `mv_min_kb=64` (a 64 KiB motion vector buffer instead of ~4.5 KiB).
- F:M016-c  **The two failure modes are not the same defect.** (1) A P or B picture is
  returned DONE but only part of it matches the reference (Y ≈ 8400 of 25344 samples).
  (2) The buffer comes back flagged ERROR and the engine's status register reads
  **0x60010003** — bit 0 finished, bit 1 error (F:M014-i), plus 0x60010000, whose
  meaning is not established.
- F:M016-d  **The MV halves were 1584 bytes apart; M014-k measured 1792 as exact for
  this size.** The driver used `wmb * hmb * 16` with the SPS width (11 x 9 x 16 = 1584)
  while the engine's MV addressing follows the picture stride (192 bytes = 12
  macroblocks) and needs one more 64-byte line. It now uses
  `roundup2((stride/16) * hmb * 16, 64) + 64` = 1792. Measured: `wb.264` 7/50 with
  1584 against 2/50 with 1792 in one pair, but 10/100 in a second run — at n = 50–100
  the improvement is suggestive, not significant, and `f3.264` is unchanged within
  noise (5/50 against 8–9/100). The change is required by M014-k regardless, and it is
  not a regression; it does **not** remove the intermittent failure.
- F:M016-e  **The driver's own placement check is violated in every slice.** Its
  `h264_last` reports `mixed == slices` for all 100% of slices: the engine's buffers
  are always spread over several 64 MiB segments (source and luma near 0x0a……, the
  motion vector buffer near 0x01d…, the two scratch buffers 0x09…… and 0x0dec……). The
  decode still succeeds most of the time, so whether the engine requires one segment is
  **not** established — the counter is a lead, not a cause.
- F:M016-f  **It is not the CPU cache in either direction.** The hardware buffers come
  from `bus_dmamem_alloc(..., BUS_DMA_COHERENT)` and the media core syncs them on
  completion (`vbq_buf_done` → `BUS_DMASYNC_POSTREAD`); forcing a full write-back +
  invalidate of every engine buffer before every slice changes nothing (F:M016-b).

- F:M016-g  **The programming is byte-identical.** The driver records every `bus_write_4`
  of a run (`dev.cedrus.0.h264_trace`, 914 writes over three slices); diffing a failing
  run against a passing one — after normalising the buffer addresses, which differ
  between runs — gives **zero differences except the value written back to 0x228**, the
  acknowledge, which is the engine's residual status read at that moment: 0x10010000 in
  the failing runs against 0x20010000 or 0x50010000 in the passing ones (bits 28..30,
  meaning unknown; bit 16 is `H264_STATUS_SKIPPED`).
- F:M016-h  **The engine reads identical inputs.** With the driver instrumented to FNV
  the reference picture's visible buffer and its motion vector buffer at slice start
  (`dev.cedrus.0.h264_ref`), twelve passing runs all give exactly
  `luma0 aea2f32e mv0 bcc31dc5 luma1 514751c7 mv1 10d4c14d`, and the three failing runs
  of a later batch give **the same four values**. The source slice data does not change
  during a slice (the driver's own FNV, no `SLICE DATA CHANGED`), and the placement does
  not discriminate: the three placement patterns seen in failing runs all occur in
  passing runs as well, and the failure rate is not higher when the source and the luma
  are in different 64 MiB segments (3.9% against 6.8%).
- F:M016-i  **Also rejected by measurement:** the acknowledge value (write back what was
  read / that value or 0x7 / just 0x7 → 5, 2 and 8 failures per 100 — noise) and the
  power gating (`hw.cedrus.pm=1`: 0 failures in 150 runs; `=0`, the engine never gated
  off: 4 in 150). `PLL_VE` (0x01c20018) reads 0x93006207 after a decode — enabled
  (bit 31) and locked (bit 28).
- F:M016-k  **The wrong content really is in the buffer.** The test now re-reads a
  mismatching capture buffer immediately and again 50 ms later, and both re-reads give
  exactly the same counts as the first read (`Y 338/25344` → 338, 338; `Y 8408/25344` →
  8408, 8408). The picture is wrong in DRAM, not stale in the CPU's view — so the engine
  produced it, and the comparison path is not the defect.
- F:M016-l  **The instrumentation is neutral.** Reading the reference buffers from the
  CPU before every slice was a candidate explanation (an observer effect that would also
  have named the mechanism); A/B on 200 runs each rejects it: with the read 2 partial
  plus 11 ERROR, without it 2 partial plus 10 ERROR. The same batch also shows the two
  failure modes side by side, so they are not exclusive to different builds.
- F:M016-m  **The lab does not reproduce it.** With the register lab restored
  (`vectors/h264/README-lab.md`), `f3` replayed through `/dev/cedrus-lab` — Linux's
  programming, our own layout in a single 8 MiB buffer at a fixed physical address, no
  V4L2, no allocator, no interrupts — came back **bit-exact in 100 runs out of 100**.
  At the driver's rate (about 5%) the chance of that is under 1%, so the engine does not
  fail on its own: the failure needs something the driver's configuration adds. The same
  lab run also answers a detail from F:M016-g — its status reads show the very bits 28..30
  that differ in the driver's traces (`0x50000001`, `0x70010001`, `0x60010001`), so those
  are the engine's normal status values, not an anomaly.
- F:M016-n  **The driver's programming equals Linux's** apart from the stride. Comparing
  the register state a slice is started with — the driver's `h264_trace` against
  `linux-trace-h264-set3.txt`, addresses normalised — leaves exactly two registers:
  0x0c4 (driver 6912 against Linux 6336) and 0x0c8 (luma stride 192 against 176). Both
  are the capture layout the test asks for (stride 192), and M014-b already noted that
  0x0c4 does not matter for NV12 output. Nothing is missing and nothing else differs.
- F:M016-o  **The engine's buffers are uncached, so no read can be stale.**
  `dev.cedrus.0.h264_last` now also reports the memory attribute of the source and the
  destination buffer, and both read **2 (VM_MEMATTR_UNCACHEABLE)**: on arm64
  `bus_dmamem_alloc(BUS_DMA_COHERENT)` does return uncached pages, and the media core
  passes that attribute on to the user mapping (`vbuf_mmap_object` →
  `vm_object_set_memattr`). So the test's reads cannot be stale and its writes to the
  slice data cannot be sitting in a cache — F:M016-k's re-read result stands, and the
  wrong picture really is what the engine wrote.
- F:M016-p  **The frame hash through the direct map is the tool for a future partial
  failure.** `dev.cedrus.0.h264_dst` hashes the visible picture the engine wrote, read
  through the uncached direct map, in completion order; for passing runs it equals the
  ffmpeg reference exactly (`a2ad9dee 2c58de87 a4c2b028` for f3's first three pictures,
  display order 0, 1, 3). If a failing run's hash matches while the test's view does not,
  the fault is the mapping; if it does not, the engine wrote the wrong picture. In the
  250 runs after the tool was added the partial mode did not occur at all (17 ERROR, no
  partial), so the tool is still waiting for its case.
- F:M016-q  **Interrupts are not the difference.** 200 runs each: 186 PASS / 3 partial /
  11 ERROR with the interrupt (the default) against 189 / 1 / 10 with `h264_poll=1`, i.e.
  the driver completing by polling the status exactly as the lab does changes nothing.
- F:M016-r  **Co-locating the engine's own buffers is not the difference either.** The
  driver's `mixed` counter is set on every slice because its motion vector and scratch
  buffers come from the low window while the pictures land high, whereas the lab's
  buffers always sit in one 64 MiB segment. A new knob (`dev.cedrus.0.aux_near`) takes
  the engine's buffers from the destination picture's own segment — verified: luma
  0x0991…, mv 0x0911…, scratch 0x0838…/0x0964… all in segment 2 — and 200 runs each give
  192 PASS / 2 / 6 without it and 187 / 3 / 10 with it. The counter stays non-zero only
  because of the **source** buffer, which the application allocates and the driver cannot
  place; that is the one part of the arrangement this knob cannot change.
- F:M016-s  **The picture stride is not the difference either.** The driver programs
  0x0c4/0x0c8 for its own capture stride (192 at 176x144, `roundup2(w,32)`), while the
  lab always programmed 176 (`(w+15)&~15`). The lab now takes a stride override
  (`H264_STRIDE`); with **192** and the driver's own values in those registers, 100 runs
  of f3 were **bit-exact, 0 failures** — the same as with 176.
- F:M016-t  **Reproducing the `mixed` arrangement in the lab is not possible as it
  stands.** The lab's own buffers always sit in one 64 MiB segment, and making them span
  a boundary needs the lab buffer itself to cross one. The driver's `hw.cedrus.lab_size`
  was raised to 64 MiB and `hw.cedrus.lab_lo_mb` swept over 56..63 and 118..127, but the
  guest's kernel occupies those regions: the buffer lands at 0x4a000000 (rel 160 MiB)
  every time, and a 64 MiB buffer does not allocate at all. So the last difference from
  the driver — a source buffer in a different segment from the pictures — cannot be
  reproduced with the lab's single contiguous buffer without changing the driver's lab
  allocation (an explicit address, or a non-contiguous buffer).
- F:M016-j  **The rate is strongly batch-dependent and the failures cluster in time:**
  0 of 150 runs in one batch, 8 of 150 in another, and inside a batch they sit close
  together (runs 22, 24, 29, then 111). Together with F:M016-g/h that says the engine is
  given the same inputs and the same programming and still produces a wrong picture
  some of the time — a condition outside the driver (clock, thermal, interference) or an
  engine-level non-determinism, not a programming defect.

- F:M016-u  **The mechanism is a timing margin, not logic.** M022 measured the engine's
  error rate against its module clock divider: 2.5% at the boot setting (297 MHz against
  a 300 MHz AHB), 12.5%, 42.5% and 100% as the clock is divided further. Identical inputs
  and identical programming, and a failure rate that is a steep function of the clock —
  the engine works against a margin, and the batch-to-batch variation fits a margin that
  moves with temperature and voltage. The default setting is the best end of that curve.
Open: the cause is not identified, but the shape of the defect is now measured from
both sides. **The engine is handed identical inputs and identical programming and, in the
driver's configuration, intermittently writes a wrong picture** — a partial one returned
DONE, or a slice that ends with an error and status 0x60010003 — while **the lab, given
Linux's programming and a single fixed buffer, is bit-exact in 100 runs out of 100**. So
the engine *can* do this correctly, and something about how the driver drives it makes it
fail about one run in twenty. Every factor tested so far is closed (F:M016-b, g, h, i, k,
l, m, n, o): programming, inputs, addresses and windows, cache flushing, delays, polling,
reset, zeroing, the acknowledge value, power gating, the MV layout, the reference reads.

Every driver-side factor tested is now closed (F:M016-b, g, h, i, k, l, m, n, o, q, r,
s), the clock lever has been taken as far as it can safely go (M022: dividing the engine's
clock breaks it, raising it to 336 MHz changes nothing, and the residual ~7% at and above
the nominal clock is rate-independent), and the spacing between slices makes no difference
either (M022-g). The lab — the same programming, the same inputs, the same clock, a fixed
buffer and no V4L2 — stays clean in 100 runs out of 100. What is left is the V4L2 request
pipeline itself as a *pattern*: per-slice requests through the media core, which the lab
does not reproduce, and which no knob of the driver changes. Nothing in that pipeline has
been shown to be wrong; it is the only difference that remains.

Artifacts: `tools/run-h264-test.sh` (the run), the driver's `h264_last`/`rejects`/
`errors` counters, and this file. The numbers above come from single sessions on
2026-10-06; the sampling noise at n = 50–100 is of the same order as the differences
between configurations, which is why the rejected knobs are listed as rejected rather
than as small effects.
