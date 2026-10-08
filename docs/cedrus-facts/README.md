# Cedrus (A64 video engine): clean-room fact sheets

The BSD-2 driver in `freebsd-media/sys/dev/cedrus` must not be a translation of
GPL code. Rules (decision Q3 in `docs/specs/SPEC_media_v4l2_native.md`, option 1):

**Allowed sources of facts**
1. Public documentation: the linux-sunxi wiki (the archive `sources/` described below is NOT in this repository; licence of the wiki not re-verified, see `docs/AUDIT.md` F-04),
   the A64 user manual.
2. Measurements on the board: register dumps, reset values, which bits stick,
   behaviour of the engine on streams we generate ourselves. Recorded in
   `measurements/` with the exact procedure and the output.
3. The V4L2 UAPI headers (BSD-3 option) for the control structures.

**Not allowed**: reading, copying or paraphrasing the source of the Linux
`cedrus` driver, libvdpau-sunxi, libva-v4l2-request, or any other GPL/LGPL
implementation of the engine's programming sequence. A sequence we need is
found by experiment against a reference decode (ffmpeg software output) and
written down as a measurement.

**Provenance rule for code**: every register, field and sequence step in the
driver carries a comment `F:<id>` naming a fact below (`sources/` section or a
`measurements/` file). A line that cannot be traced to a fact is not
acceptable.

**Limits of this scheme, stated plainly**: the author of the driver is a
language model that has seen public code, including GPL drivers for this very
block, during training. The scheme cannot erase that; what it adds is an
auditable trail: reviewers can check that every register fact comes from the
wiki or from a measurement, and that sequences were derived experimentally
with a reference output to compare against.

## Layout
- `sources/`      archived primary sources (verbatim text, with attribution)
- (`ve-register-map.tsv`, a register table extracted from the linux-sunxi wiki, was removed 2026-10-08: its source terms are unknown; it stays in git history only. The driver's register facts rest on the `measurements/` notes.)
- `measurements/` experiments on the board
