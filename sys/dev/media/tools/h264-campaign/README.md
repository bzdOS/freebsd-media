# h264-campaign: the toolbox for the field / MBAFF / all-scenarios work

Host side, in `/opt/bzdos/linux-work/` (not in git: sources of the Linux kernel, the traces, the
downloaded corpus). See bsdOS/docs/cedrus-facts/HANDOVER.md for the full story.

- `catalog.py`, `refcheck.py`: unzip the JVT conformance suite in `linux-work/jvt/{AVCv1,FRExt,
  Professional_profiles}/*.zip` into `linux-work/jvt/streams/<name>/`, record features (SPS flags, field,
  MBAFF, profile, chroma, bit depth) in `catalog.json`, and check ffmpeg's decode against the yuv the
  zip carries (173 of 231 match; the rest have no yuv or are 10-bit/4:4:4/FMO).
- `cut.sh <name> <n>`: first n packets (fields count as packets) of a catalogued stream, Annex B.
- `dump.c`, `canon.c`: host programs over `tests/h264ctl.h`: print what the front end produces
  (pictures, DPB entries, slice lists). `canon.c` with the old header (git show 26e72f5:...) and the
  new one gave identical output on all 37 frame streams: the rewrite did not change frame decoding.
- `dpbcmp.c` + `dpbcmp.py` + `runall.sh`: compare the DPB of the front end with ffmpeg's own
  (`ffmpeg -debug pict+mmco`: short/long term lists). 170 of 181 supported corpus streams agree
  completely, including all PAFF/MBAFF ones that align; the 11 left are exotic (sub-sequences, lost
  slices, MMCO in MR1_BT_A and CABACI3): not looked into.
- `init_h264.c`, `mkhdr.py`: the Linux register-trace rig's /init (field aware: HOLD_CAPTURE_BUF across
  the two fields of a frame, one hash per frame, flags word in the PIC line).
  Experimental variant 5 clears only DPB LONG_TERM flags to measure their Linux programming effect;
  its output is not a conforming decode claim.
- `run-cycle.py <tag>`: agent-operated TFTP cycle, exclusive board lock, new artifact directory
  `linux-work/h264-traces/<tag>/`, input/staged-image MD5 manifest, console and complete trace.
  Requires host raw Ethernet and guest SSH access. No persistent-media flashing. After staging,
  failures and handled signals restore the FreeBSD TFTP pairing and request watchdog reboot.
  Success requires SSH to FreeBSD with UFS `/var`, within 240 seconds after reset. Exit is nonzero
  for failed capture or recovery; `result.json` distinguishes them. SIGKILL/host failure cannot
  run cleanup; restore the manifest's FreeBSD pairing before manually resuming a failed cycle.
  Source hashes record the files present at capture, not proof that each was embedded in Image.
  Stop the FreeBSD console follower before this cycle: concurrent EMAC readers interfere.
  Completion polling reads a short console tail; the full console is collected once afterward.
  On a completion deadline, partial console/trace artifacts are retained before recovery.
  The trace capture does not certify decoding: run `verify.py` separately.
- `verify.py <console> <exact-embedded-clip> ... --output <comparison.json>`: require every
  expected PIC and compare uncropped 8-bit luma FNV1a against ffmpeg in signed POC order.
  Supports short variant-0 clips with one POC epoch; refuses later IDRs/MMCO5 and other formats.
  Does not compare chroma. It exits nonzero for rig errors, count errors or hash mismatch.
  `--completed-prefix` checks only the explicitly supplied completed streams before a later
  failure; the JSON marks that restricted scope and does not certify the whole rig run.
- `trace-summary.py <trace> <output.json>`: measured register values, SRAM frame-table words,
  MV-half distances and reference-list words per slice. No Linux implementation source needed.
- `export.c`: build with host Linux UAPI and `-I../../tests`, then
  `export stream.264 output/prefix > output/prefix.json`. Writes one file per request slice and
  field/frame metadata with frame-shared timestamps. `canon.c` uses those same timestamps.
- `inspect.c`: build with the same includes; `inspect stream.264` reports complete-stream
  strict parser acceptance, field/MBAFF counts, long-term references and explicit B weights.
  This inventories coverage, without claiming pixel correctness. Profile number alone must
  not exclude streams: four Extended-profile corpus streams use supported syntax.
- `replay.py trace prefix.json stream.264 output-dir --first-run N --mv-factor 2`:
  relocate the measured writes into the 8MiB FreeBSD lab, hold paired fields in the same buffer,
  acknowledge completion before each slice, check guest uploads, compare full NV12 and inspect MV
  canaries and each final engine status. `--clear-modes` is the one-variable negative control.
  Factor 1 is for progressive direct8=1; interlaced and progressive direct8=0 need factor 2.
  Only single-POC-epoch clips fit this reference mapper.
  `--canon prefix.canon.txt --long-term-mask 0x11` reproduces the M027 long-term
  experiment: only low bits of word 2 in LONG_TERM SRAM entries change. It cannot
  combine with `--clear-modes`; buffer addresses and field/MBAFF mode bits stay intact.
- `jm-list-compare.py --canon DIR --jm DIR --output result.json NAME ...` compares
  `canon.c` output with independently observed JM `ORACLE` records from
  `jm-lists-NAME/decode.txt`. M027 documents the observer and its validated YUV output.
  Current-picture identity and ordered reference POC, structure and long-term
  flag/index must agree on every slice. Raw reference frame_num differences remain
  reported separately (JM split-field entries can report zero). Empty or unequal
  record counts are errors. This comparator is scoped to one POC epoch and does
  not certify the other controls or decoded pixels.
- `board-corpus.py output-dir --frames 0`: sequential complete-stream comparison over catalogued
  8-bit 4:2:0 streams without initial slice groups; `--interlaced` filters to 73 streams,
  `--frames 8` uses short cuts, `--names ...` selects regressions. Every upload is MD5-checked and
  every reference frame must have a PASS line. Separate logs retain ffmpeg warnings and decoder
  errors. It requires the already-built `cedrus_h264` and loaded driver; it never installs a module.
  Catalog filtering examines the initial parameter sets; later FMO PPS changes and monochrome
  streams reported as yuv420p still need classification from the parser/probe logs.

The agent owns build, deployment, capture, comparison and recovery. Use one `make Image` under
`linux-work/build-min/.h264-campaign-build.lock`; build before acquiring the board for a cycle.
Keep the console before a FreeBSD module test and MD5-check every guest upload. Investigate using
`triage.py` first, and never resolve callable addresses from an ELF with a different live build ID.

M027 regression: **174/174 parser-accepted streams exact (18,828/18,828 frames)**.
The measured LONG_TERM table encoding fixes MR8_BT_B, MR9_BT_B and MMCO4; JM
independently confirms their reference lists. Data DMA defaults now span 1GiB.
Two full corpus passes plus probes run on one guest boot without allocation failures;
the rebuilt module ends with zero engine errors/timeouts and zero live vbq allocations.
57/231 streams remain outside the strict parser's format/syntax scope. See M027
for the complete matrix, lab guards, short-window A/B limits and module provenance.
