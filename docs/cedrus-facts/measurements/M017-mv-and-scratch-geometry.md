# M017: MV and scratch buffer geometry, from the Linux traces (2026-10-07)

Method: read the addresses Linux programs out of the committed vetrap traces — set2
(`bt b3 db smb wb hd640 hd720 highp cip ref8 cqmp`) and set3 (`f1 f2 f3`) — through
`join.table()` (`vectors/h264/join.py`). The picture table's words 5 and 6 (SRAM 0x400)
are a picture's two motion vector halves; 0x250 and 0x254 are its two scratch buffers.
No board was needed for any of this.

- F:M017-a  **The two MV halves are `wmb x hmb x 16` apart, with the SPS width.** Linux
  programs `0x630` (**1584**) at 176x144 — the same value in five independent streams
  (wb, smb, b3, f1, f3) — and `0xe100` (**57600**) at 1280x720, which is 80 x 45 x 16.
  The picture *stride* does not enter: our 176-wide pictures have a 192-byte stride and
  Linux still used 1584.
- F:M017-b  **M014-k's "3456 corrupts, 3584 is exact" is the size of the *per-picture*
  MV buffer, not the spacing between the halves.** The lab allocates one 4096-byte slot
  per distinct MV buffer ("4096 apart (Linux's spacing)") and the M014-k experiment
  varied that slot; 3456 was too small and 3584 exact, which means the engine writes
  past a bare half — a picture needs about 2 x 1792 bytes at 176x144 — while the halves
  themselves stay 1584 apart. The driver's per-picture allocation,
  `MAX(wmb*(hmb+1)*32 + 1024, mv_min_kb << 10)`, is 4544 at 176x144 and 118784 at
  1280x720, i.e. above 2 x 1792 and above 2 x 57600 respectively, so it satisfies the
  requirement with room to spare.
- F:M017-c  **Linux's per-picture MV buffers are allocator-rounded, not a formula:**
  4096 at 176x144, and `0x80000` (512 KiB) between consecutive pictures of hd720
  (1280x720). The driver's tight sizing is therefore *below* Linux's but still above the
  measured requirement (F:M017-b).
- F:M017-d  **The scratch buffer size cannot be derived from the traces.** The two
  addresses are independent allocations in Linux — hd720: 0x250 = 0x65c0000 and
  0x254 = 0x6120000; hd640: 0x250 = 0x6580000 and 0x254 = 0x6120000 (the second one is
  the same buffer in both streams) — so their distance says nothing about the size. The
  driver's `MAX(128 KiB, wmb*hmb*1024)` (3.5 MiB at 1280x720) remains an unverified
  guess, and M014-k's "128 KiB each works at 64x64 to 176x144" is the only measurement.
- F:M017-e  **The driver's half spacing is reverted to Linux's value.** It had been
  changed to a stride-based value with a 64-byte pad (`fbsd-media` `a47da73`) on a
  misreading of M014-k; it is `wmb*hmb*16` again. Measured with the revert: f3.264,
  100 runs — 95 PASS, 5 with the buffer flagged ERROR, no partial picture. The spacing
  is not what the intermittent failure turns on (M016).
- F:M017-f  **A third failure mode exists.** While verifying, `smb.264` (15 slices per
  picture) ended with `picture 2 slice 6: request did not complete` — a slice that does
  not complete within the driver's job timeout. That is neither the partial picture nor
  the engine-reported error of M016, and it is the first time it has been seen; smb
  passed 3/3 in earlier runs.

Artifacts: this file; `vectors/h264/join.py` (the table extraction) and the traces it
reads. The numbers above are single measurements of a trace, not statistics.
