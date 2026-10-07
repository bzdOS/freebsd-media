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
