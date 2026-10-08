# M037 — exact FMO PCM with one macroblock per slice

2026-10-08. The known slice-start coordinates correctly place every block of
a valid interleaved FMO picture **when each slice contains exactly one PCM
macroblock**. No traversal map is needed inside such a slice. This is a new
narrow hardware positive; M033's multi-macroblock FMO failure remains unresolved.

## Input and independent validation

Two352x288 eight-bit420 IDR pictures have396 single-I_PCM-macroblock slices.
Both contain the same patterned Y/U/V samples. The raster control has no slice
groups and orders blocks0..395. The FMO input has two interleaved groups,
map type0 and run lengths1/1, with even addresses0,2,..394 followed by odd
addresses1,3,..395. Each original slice has its real absolute `first_mb_in_slice`.
No input rewriting, CPU pixel reconstruction or host output placement is used.
[Generator](M037-fmo-one-mb/fixtures.py),
[validation](M037-fmo-one-mb/validation.json).

The external JM decoder initially rejects the raster control at its configured
50-slice implementation limit. Only `MAX_NUM_SLICES` in that external oracle
is increased to512; its make exits0. Decoder/header provenance and that exact
one-constant change are recorded in
[oracle-limit.json](M037-fmo-one-mb/oracle-limit.json).
Both complete396-slice streams then decode **exactly** to the generated planar
references. Exact decoder commands and JM logs are saved per case. This oracle
limit adjustment is not a decoder algorithm copied into the BSD driver.

## Hardware commands and result

The lab retains M033's measured420 PCM contract. Each slice uses the observed
bit-reader init7, header skip3 in chunks of at most32bits with busy-bit waiting,
and slice decode8. Start coordinates in register0x208 come from the absolute
block address. There is no invented per-macroblock opcode or unknown trigger
sweep. All output goes into one shared capture buffer; the host only compares
the finished picture. Each slice contains one block, so decode termination
provides the per-block boundary.

| Case | Slice completions | Full152064-byte NV12 | Guards | Untouched output tail |
|---|---:|---|---:|---|
| Raster control |396/396|exact|4/4|yes|
| Interleaved FMO |396/396|exact|4/4|yes|

Both lab processes exit0, no timeout, every low-three-bit status is1, and every
output/MV guard is intact. [Results](M037-fmo-one-mb/results.json). Each case's
`probe.lab` and `lab.txt` retain all396 sequences and observed statuses.
There are **two executions and eight intact guard observations**.

This proves slice-start placement for tested CAVLC PCM slices with FMO ordering.
It does not establish multi-MB group traversal, motion/reference/neighbour
semantics, arbitrary FMO maps, CABAC groups, or the three official FMO streams.
The production parser/driver continues to refuse general FMO. A follow-up can
use this positive control to distinguish start-coordinate errors from the
missing inside-slice traversal contract; repeating M033 without a map remains
insufficient.

## Native recovery after M035–M037

The canonical series totals **44 executions and227 intact guard observations**
(M03515/30, M03627/189, M0372/8), in addition to the Linux capture and the
separately preserved adapter diagnostics. Afterwards the unchanged native
module is reloaded and the ordinary test passes f3 **8/8**, weighted mono
**36/36**, and mono MBAFF/CABAC **36/36**, all full Y/C exact.
[Recovery tool](M037-fmo-one-mb/recovery.py),
[results](M037-fmo-one-mb/recovery.json),
[state](M037-fmo-one-mb/final-state.txt).

Final native counters: pictures80, errors0, timeouts0, rejects0, missing_refs0;
vbq live0, cedrus_dec one1KiB allocation. The enabled lab intentionally retains
8MiB. Lab errors are separate from native job counters. All guest uploads are
MD5-checked. Module MD5 is still d57627e64bb4cfc21e64d264e9a19b5a;
`fbsd-media f4450d4`, production M028 unchanged. The three live FreeBSD TFTP
files match their backups: [checks](M037-fmo-one-mb/tftp-restored.json).

Console follower PID3249662 was stopped after the owner requested a status
and stop. Its capture remains at
`linux-work/h264-traces/m035-sp-minimal/console-after-linux.txt`; start a fresh
follower before another module test. The
running hypervisor's build5137e4fca675 differs from the local ELF d1c6f75d9105;
do not call functions using symbol addresses from that local ELF. Working
M037 artifacts: `linux-work/h264-traces/m037-fmo-one-mb/`, raw buffers indexed
by [raw-sha256.json](M037-fmo-one-mb/raw-sha256.json).

Production acceptance remains M028's176/176 streams,19028/19028 full Y/C frames,
with M030's mono-PCM caveat. The remaining55 official streams are27 ten-bit,
13 fourteen-bit,10 eight-bit422,3 FMO and2 SP. No new production capability,
software fallback, push, or claim of universal silicon impossibility follows
from this series. The complete hardware-only request remains open.
