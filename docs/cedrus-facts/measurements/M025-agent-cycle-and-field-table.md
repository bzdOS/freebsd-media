# M025 — agent-operated cycle and field-table observations

Date: 2026-10-08. Four variant-0 clips from M024. No Linux decoder implementation
source was consulted.

## F:M025-a — recovery is a measured gate

Versioned `fbsd-media/sys/dev/media/tools/h264-campaign/run-cycle.py` replaces the
scratch shell runner. The board lock covers capture and recovery. Existing run
directories and an unexpected initial TFTP pairing are rejected. After staging,
failures and handled signals trigger restoration and watchdog reboot. Success
requires FreeBSD SSH and UFS `/var` within 240 seconds after reset.

Live run `linux-work/h264-traces/field1-agent-cycle`: staged 00:44:47 MSK,
captured 4,398 entries with isv0=0, restored TFTP 00:45:19, confirmed FreeBSD
15.1-RC3 and `/var` on `/dev/vtbd1p1` at 00:46:09 after six unsuccessful SSH
probes. No user intervention. `verify.py` regenerated ffmpeg references and
matched **17/17 luma hashes**. Chroma was not compared.

Artifacts: [cycle](M025-agent-cycle/cycle.txt),
[manifest](M025-agent-cycle/manifest.json),
[capture/recovery](M025-agent-cycle/result.json),
[luma comparison](M025-agent-cycle/comparison.json),
[restored guest](M025-agent-cycle/guest-after-7.txt).
The capture result leaves `decode_validated:false`; the separate comparison
records luma success. Source hashes record capture-time files, not proof that
they were embedded in Image. SIGKILL/host failure cannot execute cleanup.

Fault-injection checks covered delayed SSH, failed reset and expired recovery.
Preflight triage capture and a build-lock-protected Image snapshot were added
after this live run; those additions have not had another hardware cycle.

## F:M025-b — observed modes and field sharing

`trace-summary.py` extracts the M024 trace into
[20 slice summaries](M025-agent-cycle/field1-summary.json). Front-end dump mapping:
4 CVPA pictures, 4 CAMA pictures, 4 CAPAMA pictures, 8 f3 pictures; one slice each.

- SRAM table `0x400`, eight words per slot: word 2 is `0x100` for field pictures
  (runs 0–3, 9–10), `0x200` for MBAFF frames (4–8, 11), zero for progressive
  frames (12–19). Mixed run 11 retains each reference's respective mode.
- Word 0/1 holds top/bottom POC, including negative values as two's complement.
  The second field retains the same slot/output addresses and updates bottom
  POC. No per-field output-base shift is observed in these entries.
- MV word 6 minus word 5: `0x3180` (12672) for 352x288 field/MBAFF and `0x630`
  (1584) for 176x144 progressive. **Correction (M026): the original formula
  claiming both equal `width_in_mbs * full_frame_height_in_mbs * 16` was an
  arithmetic error.** The interlaced half is twice that: `22 * 18 * 32 = 12672`;
  the progressive half is `11 * 9 * 16 = 1584`. This measures half spacing,
  not allocation size or engine write bounds. M026 tests bounds with canaries.
- CVPA top-field list in run 2 has bytes `00 01`, bottom-field run 3 has
  `01 00`. CAPAMA run 10 has `01 02 00`. These are consistent with low-bit
  bottom-field selection; confirm slot mapping against frame-shared timestamps
  and controlled replay before treating this as a complete encoding contract.
- Register `0x200`: `0x00091508` for CVPA, `0x000b1508` for CAMA/CAPAMA,
  `0x000d0a08` for f3. Cross-stream differences do not isolate individual bits.

Next: canonical control dump with the rig's frame-shared timestamps, then
field-aware lab replay of table modes, list bytes and MV bounds. The FreeBSD
driver still rejects fields/MBAFF; no acceptance change is claimed.
