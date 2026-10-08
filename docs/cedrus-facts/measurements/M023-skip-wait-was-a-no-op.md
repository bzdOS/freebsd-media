# M023: the intermittent H.264 engine error — the header-skip wait was a no-op (2026-10-07)

Method: the per-slice error rate as an assay. The driver counts every slice it starts
(`h264_last`, `slices`) and every slice the engine ends with an error (`errors`); eight
runs of `smb.264` (15 slices a picture, stops at the first error) give ~50-450 slices per
configuration, enough to tell 12% from 0% in one go. A positive control: two `dd
if=/dev/zero of=/dev/null bs=8m` processes on the guest ("hog") load the DRAM.

- F:M023-a  **The error is a per-slice event of about 2.7%, not a per-run one.** `smb`
  (45 slices a run) failed in 22 of 30 runs at HEAD (F:M016-a said it had
  not been seen to fail; how that was counted is not known — a run that stops on an
  error also prints `QBUF cap: Invalid argument` for the picture after it, so a count of
  only the FAIL lines or of the exit status can miss it; here a run counts as good only
  if every picture line is PASS and none of FAIL, ioctl errors or timeouts appears). `f3` (3 slices) failed
  in 1 of 30. Both fit 2.7% a slice. The HANDOFF's 6-7.5% was `f3` only.
- F:M023-b  **It depends on DRAM load, not on CPU load.** Idle 2.7%; two CPU spin loops
  2.7%; two DRAM-writing `dd` 11-19%. The lab (F:M016-m) stays clean under the same hog:
  30 replays of `f3` (240 slices) with 0 errors, where 19% would give about 40.
- F:M023-c  **Not a knob of F:M016.** Under the hog: polling instead of the interrupt,
  `aux_near`, the cache flush, scratch 2 MiB, MV 4 MiB — all 11-20% like the baseline.
- F:M023-d  **Spacing the register writes cured it, and three single registers each did:**
  a 2 us pause after every write — 0 errors in 360 slices. After only `0x050`, only
  `0x224` (the commands before the start) or only `0x230` (the VLD address) — 0 in 360;
  after any of the others (`0x000`, `0xec`, `0xc4`, `0xc8`, `0x2e0`, `0x220`, `0x228`,
  `0x238`, `0x23c`, `0x240`, `0x24c`, `0x250`, `0x254`, the start command, the SRAM
  data port) — 8-11 errors in 8-10 runs. `0x230`, `0x050`, `0x224`=7 and the first skip
  are consecutive: the time between loading the bit stream address and the first skip
  command is what matters.
- F:M023-e  **The wait after a skip command waited for a bit that is always set.**
  F:M014-d took bit 16 of `0x228` as "the skip is done". In every one of Linux's reads of
  `0x228` after a skip (`linux-trace-h264-set3.txt`) bit 16 is already 1 — it is a
  constant, so the driver's loop returned at once and the next command went out while the
  bit stream reader was still fetching from DRAM (its start address was written a
  microsecond earlier). The reader's busy flag is **bit 8**: waiting for it to clear
  (what Linux's driver does as well) found the reader busy at the first read in 906 of
  ~2250 slices' skips under load. With the change: **0 errors in 2250 slices under the
  hog** (50 runs of `smb`), against 10 in 81 with the old wait, same build, switched with
  `dev.cedrus.0.h264_oldskip`. Idle: `smb` 40/40 and `f3` 40/40 runs complete, where 8/30 and
  29/30 did.
- F:M023-f  **What this explains in the earlier files.** The engine error was never a
  timing margin of the clock (M022): a slower module clock only made the fetch slower, so
  the race was lost more often (2.5% -> 100%). The lab was clean because each lab write is
  a system call with microseconds between them, and Linux's trace because the hypervisor
  trapped each access. The batch dependence (F:M016-j) is the DRAM traffic of the
  moment (the display the hypervisor owns, the debug channel). Nothing in the V4L2
  request path was involved.
- F:M023-g  **The later pictures of eight streams: a bug of the test, not of the driver.**
  Run to the end (no `-n 3`), `b2 b2_cavlc b3 bt cqmp f2 p3 p_cabac` came back with a few
  dozen wrong luma macroblocks (chroma exact) from the fourth picture on, the same way in
  every run. The test picked the capture buffer for a picture from its DPB *after* parsing
  that picture; the sliding window had already dropped the oldest reference, so that
  reference's buffer looked free and the picture was decoded over its own reference. The
  driver's table showed it: the current picture and reference 0 had the same luma address
  (f2, picture 7). The register programming was otherwise equal to Linux's, slice by
  slice (ordering of the writes, skip commands, lists and tables modulo addresses). The test
  now skips every buffer the picture's own decode parameters name. Result: **all 33 committed
  streams (the 25 of M021-a and `f1 f2 f3 wb smb hd640 hd720 t640`), every picture to the
  end, 99 of 99 runs exact at idle** and 98 of 99 plus one `REQBUFS cap: Cannot allocate
  memory` at `hd720` under the DRAM hog (20 contiguous 1.4 MB buffers when memory has been
  fragmented by the run — an allocation limit, not a decode error).
  An application must do the same: a picture's destination must not be a buffer in its
  own `dpb[]`. The driver substitutes the picture's own buffer for a reference it cannot
  find (`missing_refs`) and does not look for this case.

Artifacts: this file; `dev.cedrus.0.h264_oldskip` (1 = the old wait, for an A/B) and
`dev.cedrus.0.skip_busy` (skips that found the reader busy).
