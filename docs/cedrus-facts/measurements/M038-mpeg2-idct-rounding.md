# M038: MPEG-2 `play.m2v` — the ±1 sample drift is IDCT rounding, not a driver fault (2026-10-08)

Method: the 60-picture CIF clip `play.m2v` (I/P, 352x288) through `tests/cedrus_mpeg2 -a` on the board against
references made by ffmpeg with each of its IDCT implementations (`-idct auto|int|simple|simplemmx|xvid`);
a picture counts as exact only if every Y and C sample equals.

- F:M038-a  **The MPEG-2 test has always compared with a tolerance of ±2 per sample** (`tests/cedrus_mpeg2.c`;
  the hardware's IDCT is not bit-exact with ffmpeg's: max difference 2 on every stream, 3000-11000 samples of a
  picture differ by 1 or 2; M039 measured it with a plain `cmp`: n176, a single I picture, 590 of 38016 samples).
  What the earlier notes called "exact vs ffmpeg" for MPEG-2 means "within ±2". With that tolerance the hardware passes 24 of
  the 60 pictures of `play.m2v`; in the other 36 a few samples (4 of 50688 chroma, 1 of 101376 luma) are further
  off (`Y 101375/101376`, `C 50687/50688`), growing slowly along the P chain
  (about 2–5 samples per picture by picture 7–9). The same with the pre-M023 module and with HEAD: not an H.264-era
  regression.
- F:M038-b  ffmpeg's own IDCTs disagree with each other far more than the hardware disagrees with any of them:
  `int` and `xvid` differ from the default in all 60 pictures (262 389 and 270 664 samples in total) while the
  hardware differs from the default in 36 pictures by about 100 samples in total. `auto`, `simple` and `simplemmx`
  are identical. Against `int` the hardware fails 41 pictures, against `xvid` 44, against the default/`simple` 36.
  No ffmpeg variant gives 60 of 60.
- F:M038-c  Conclusion: MPEG-2 does not define the IDCT bit-exactly (ISO 13818-2 Annex A asks for IEEE 1180
  accuracy), so a sample-level difference of this size after a long P chain is expected from a different rounding
  of the transform. It is not a driver fault and not a regression; the 40 other MPEG-2 streams (short, mostly
  I-heavy) stay within ±2. A bit-exact MPEG-2 reference for this engine would have to be the engine's own, which the
  project does not have. Not pursued further.
