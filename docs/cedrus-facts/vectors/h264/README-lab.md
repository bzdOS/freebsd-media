# The H.264 register lab — how it is put together (restored 2026-10-07)

What it is: replay the register programming of a Linux decode through
`/dev/cedrus-lab`, with **our** buffer layout, and compare the result with ffmpeg's
decode. This is the method M014 used. The scripts used to point at a scratchpad
(a scratch directory) that no longer exists, so the chain is written down here and the
paths are derived from the file's own location instead of being hard-coded.

## Layout

Everything below is relative to this directory (`vectors/h264/`):

| File | What it is |
|---|---|
| `feat.c` → `feat` | one line of slice features per slice: `cc -O2 -o feat feat.c -I.` |
| `split.c` → `split` | one file per picture and slice: `./split <stream>.264 sl/<stream>` writes `sl/<stream>_<k>_<s>.bin` |
| `dump3.c` → `dump3` | the display order of a stream's pictures |
| `tr.py` | parse a vetrap trace (`<n> cpu0 W32 VE <off> <val>`) into per-slice runs |
| `join.py` | combine the trace with `feat`'s output into one row per slice |
| `hlab.py` | build the lab script for a run; `OFF` holds our buffer layout |
| `rp.py` | run a whole stream: build, push, execute, fetch the dumps, compare |
| `model.py`, `chk.py` | check the measured register formulas against the traces |
| `cmp_drv_linux.py` | compare the state a slice is started with as the **driver** programs it (`dev.cedrus.0.h264_trace`) against Linux's trace |

Traces: `linux-trace-h264-set{1,2,3}.txt`; the stream orders are in the streams
directory (`order.txt`, `order2.txt`, `order3.txt`). Working files (`sl/`, `x_*.lab`,
`out_*.bin`) are written next to this directory, i.e. into its parent (`SC`/`ROOT` in
the scripts).

## What the board needs

- the driver loaded **with the lab enabled**: `kenv hw.cedrus.lab=1; kldload cedrus.ko`
  → `/dev/cedrus-lab`, an 8 MiB buffer whose physical address `dmesg` prints;
- `ve_lab` built in the guest from `fbsd-media/sys/dev/cedrus/lab/ve_lab.c` together
  with `cedrus_lab.h` **one level up** (the source includes `"../cedrus_lab.h"`):
  `cc -o ve_lab ve_lab.c` — no other headers are needed;
- `/tmp/ve/ve_lab` on the guest (the scripts push the script and the slice files there).

## Run it

```sh
cd vectors/h264
cc -O2 -o feat feat.c -I. && cc -O2 -o split split.c -I. && cc -O2 -o dump3 dump3.c -I.
for s in f1 f2 f3; do ./split /opt/bzdos/linux-work/initramfs-h264/streams/$s.264 sl/$s; done
python3 -c "import sys; sys.path.insert(0,'.'); from rp import *; \
            replay('f3', trace='multi3.trace.txt', order='order3.txt')"
```

`replay()` prints `++++…` (one sign per picture) and the sample counts per picture.

Verified 2026-10-07: `f3` replays **8/8 pictures bit-exact**, and 25 consecutive runs
were all exact. The same day, `cmp_drv_linux.py` showed the driver's programming to
differ from Linux's in exactly two registers — `0x0c4` (6912 against 6336) and `0x0c8`
(luma stride 192 against 176) — which is our capture layout (stride 192), not a defect.
