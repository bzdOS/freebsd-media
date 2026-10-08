# M024: first Linux PAFF/MBAFF trace and frame hashes (2026-10-08)

Method: build the prepared Linux 6.12 initramfs rig from HANDOVER section 7.4,
boot it through the existing TFTP cycle, capture the hypervisor's VE register
trace and console, then restore FreeBSD. No Linux decoder implementation was
read. The FreeBSD driver was not changed.

- F:M024-a **The prepared Linux rig builds and runs.** One `make -j8 Image`
  completed with exit 0. Image: 6,737,928 bytes, MD5
  `841e307fb3b3571a1d39e79a21046666`. The initramfs archive's `/init` equals the
  prepared 265,584-byte executable. Cycle `field1` reached `TRACE-DONE`; the
  trace contains 4,398 entries (`isv0=0`), below even the old ring capacity.
- F:M024-b **All 17 emitted frame-luma hashes match ffmpeg.** The three
  interlaced test inputs produced 9 matching frames; `f3` produced 8 matching
  control frames. The console contains no printed control/queue failures.
  This is a 32-bit FNV-1a comparison of luma, not a full luma/chroma byte comparison.
  It validates these short inputs, not every field-reference scenario.
- F:M024-c **Negative POC matters when comparing display order.** The rig prints
  signed POC through its unsigned hexadecimal printer: `fffffffffffffffc` is
  -4, and `fffffffffffffffe` is -2. Each of these short inputs has one POC epoch;
  sorting emitted frames by signed POC gives ffmpeg's display order. Comparing
  the raw decode order would falsely reject streams 0, 1, 2 and 3.
- F:M024-d **FreeBSD was restored, but the cycle script's success criterion is
  insufficient.** `run-cycle.sh` restored all three TFTP files, and
  `wdt_reset()` returned True. Its one SSH attempt after 45 seconds got
  `Connection refused`, yet it printed `done` and exited 0. A subsequent SSH
  check succeeded: FreeBSD 15.1-RC3, uptime about one minute, `/var` on
  `/dev/vtbd1p1` (UFS, soft-updates). MD5 of kernel, DTB and HV image matched
  `.fbsd-backup`. Autonomous runs must wait for actual recovery and return
  failure when that deadline expires.
- F:M024-e **The debug channel is available through approved host execution.**
  A sandboxed `triage.py` fails creating `AF_PACKET/SOCK_RAW`; an approved
  execution outside the sandbox succeeds. Initial triage showed moving EL2
  events and a build-ID mismatch: running `5137e4fca675`, local ELF
  `d1c6f75d9105`. The cycle uses fixed-address safe-mode controls, a clean
  FreeBSD SSH reboot, and the default address-independent watchdog reset;
  no function address resolved from the mismatched ELF was called.

| Entry | Input | Size | Pictures submitted | Complete frames / matching luma hashes |
|---|---|---|---:|---:|
| 0 | `CVPA1_TOSHIBA_B.4` (PAFF) | 352x288 | 4 | 2 / 2 |
| 1 | `CAMA1_TOSHIBA_B.4` (MBAFF) | 352x288 | 4 | 4 / 4 |
| 2 | `CAPAMA3_Sand_F.4` (PAFF + MBAFF) | 352x288 | 4 | 3 / 3 |
| 3 | `f3` (frame control) | 176x144 | 8 | 8 / 8 |

Input MD5s, in entry order:

```
02593ea87e0228d74910b7fb42cacf86  field1-stream0.264  53232 bytes
fbe97e0fcc6257f4b3a1fdecfce07b0f  field1-stream1.264  91547 bytes
796d3cbfe5bbbd8bd5b0ecc3185605a5  field1-stream2.264  42016 bytes
506da40c15958179d08cdc3d4b3429f5  field1-stream3.264   7192 bytes
```

The exact embedded input bytes were extracted from `initramfs-h264/h264s.h`
to `linux-work/h264-traces/field1-stream{0,1,2,3}.264`. References were generated
with `ffmpeg -v error -apply_cropping 0 -i INPUT -vf extractplanes=y -f rawvideo -`,
then FNV-1a hashed per coded-width x coded-height luma frame (initial value
2166136261, multiplier 16777619, reduced to 32 bits after each byte).

Committed artifacts in [M024-field1/](M024-field1/): raw console, register trace,
cycle log, ffmpeg reference hashes and signed-POC comparison results.
Original working artifacts remain in `/opt/bzdos/linux-work/h264-traces/field1.*`
and `field1-*.json` / `field1-stream*.264`.

Next: decode the field/MBAFF register and SRAM differences against the canonical
controls, then extend the scenario matrix from HANDOVER 7.4.5. The FreeBSD
driver still rejects field/MBAFF pictures; this measurement does not implement them.
