# M005: general registers 0x000-0x0ff and the E11 retry (2026-10-05)

Procedure: vectors/e10-low.lab (write all ones / zero, read back), engine
select MPEG.

- F:M005-a  Writable bits in the low window (value after writing 0xffffffff):
  004:f3f70231 008:800002e6 00c:7fffff00 01c:00010100 020:80000000
  024:80000000 028:8000037b 02c:00000003 050:0000000f 054:fffffc00
  058:fffffc00 0c4:0ffffff8 0c8:ffffffff 0cc:ffffffff 0d0,0d4,0d8:fffffc00
  0e8:fffffff8 0ec:00000077 0f0:16890040 (read-only, version). Everything else
  in 0x010-0x0c0 reads zero and does not hold a value.
  054/058/0d0/0d4/0d8 are 1 KiB-aligned address registers.
- F:M005-b  vectors/e11-extra.lab = E2 plus those address registers pointed at
  free buffers and 0x050=0xf: identical outcome to E2. STATUS goes 0xc000 ->
  0xc001 within 0 ms; no byte in any output/auxiliary buffer changes.

Conclusion: filling the extra address registers is not the missing step.
