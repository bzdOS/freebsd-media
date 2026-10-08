# M020: a slice error in the middle of a picture loses the request (2026-10-07)

Method: `smb.264` — 176x144, **15 slices per picture** — through the H.264 test on the
board, with the driver's counters and `dmesg`. Twelve runs of three pictures each, then
eight more with the test's request poll raised from 5 s to 15 s.

- F:M020-a  **The engine errors on a middle slice.** `dmesg` shows slices ending with
  status `0x……0003` (bit 0 finished, bit 1 error, F:M014-i) at `mb` = 21, 35, 42 and 77
  of a 99-macroblock picture — i.e. slices in the middle of the picture, not the first
  and not the last. This is the same engine-level intermittency as M016, seen on a
  multi-slice picture instead of a one-slice one.
- F:M020-b  **The request for that slice never completes.** The test polls the request
  for `POLLPRI` and gives up after 5 s: six of the first twelve runs reported
  `request did not complete`, and the test exits at that point. With the poll raised to
  **15 s** the message still appears in seven of eight runs — the completion is *lost*,
  not late. An application that waits for the request without a timeout would hang
  forever.
- F:M020-c  **The driver is not the one giving up.** `dev.cedrus.0.timeouts` stayed 0
  through all of it: the driver's own job timeout never fired, and `dmesg` shows it
  handled the error (the status print comes from the interrupt path, which takes the
  context and calls `cedrus_job_done(d, ctx, true)`).
- F:M020-d  **Where to look.** In `vm2m_buf_done_and_job_finish()` (`vm2m.c:224`) an
  error clears `hold`, so the held destination buffer of the picture is removed from
  `cap_rdy` and completed early — and that destination belongs to the *picture*, while
  the following slices' requests still expect it. A request completes only when its
  buffer count reaches zero (`vreq_buf_finished()`, `vreq.c:233`), so a request whose
  destination was consumed by an earlier slice's error has nothing left to complete it.
  The mechanism is a hypothesis; the lost completion is measured (F:M020-b).
- F:M020-e  **Impact and workaround.** Until this is fixed, a multi-slice picture whose
  engine errors on a middle slice leaves the application waiting; a decoder that does
  not want to hang must put a timeout on the request (the test does). Single-slice
  pictures (f3, wb, hd720) surface the same engine error as a buffer flagged ERROR
  instead, which the application sees — that is the M016 path.

- F:M020-g  **Fixed in the test, which is where the contract is.** After a slice comes
  back with ERROR, `tests/cedrus_h264.c` stops feeding the rest of that picture
  (`break` out of the slice loop) instead of queueing slices whose destination the core
  has already completed. Measured after the change: twelve runs of three pictures gave
  **no request left waiting** (against five to seven per ten before), and the failed
  pictures are reported as buffers flagged ERROR — nine of them in those twelve runs,
  which is the M016 engine error, now visible to the application instead of hanging it.
- F:M020-f  **The guest rebooted twice while these measurements ran.** Two runs of the
  smb loop were interrupted by the guest coming back with a fresh uptime (and an empty
  `/tmp`, which is how it was noticed: the modules and the test binary were gone). The
  cause is **not established** — a guest panic and the hypervisor's reachability
  watchdog both look the same from the host — but it did not happen during the f3/wb
  work, only around the multi-slice error path. Recorded because it is the reason
  `tools/board-bootstrap.sh` exists.
- F:M020-h  **Fixed in the core (fbsd-media 4ebf1cb).** `vm2m` now marks the context
  orphaned when a slice that asked to hold the capture buffer fails, and completes the
  following sources with an error (no engine run) up to the one that does not ask to
  hold, or until a destination for the next picture is queued. `cedrus_h264 -t 30 -k`
  (every slice cut, the application keeps feeding the picture) used to end with
  `picture 0 slice 1: request did not complete`; it now ends every request and reports
  both pictures as ERROR (three runs). vtest and vconv 53/53, v4l2-compliance on cedrus
  46/46, the 33 H.264 streams and the MPEG-2 set unchanged. An application that feeds
  the next picture's slices *before* queueing its capture buffer would still have them
  dropped: the contract is the usual one, a destination first.

Artifacts: this file; `smb.264` and `smb.yuv` in `linux-work/initramfs-h264/streams/`;
the counters and `dmesg` above. The engine error itself is M016's open question; this
file is about what the media core does with it.
