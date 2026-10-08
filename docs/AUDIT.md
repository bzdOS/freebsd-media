# Provenance and licence audit (block E), 2026-10-08

Scope: every file in the repository at main 4bdd515 (1427 tracked files, 56 commits, all by one
author, 2026-10-05 .. 2026-10-08). Method: file inventory, SPDX scan, diff of the copied headers against the
Linux 6.12 *UAPI* headers (`include/uapi/linux`, the published interface), reading of the facts files,
cross-check of `sys/dev/cedrus/cedrus_dec.h` against the facts files and `ve-register-map.tsv`, git history.
No Linux driver source was read (not needed for any check below). Nothing here is legal advice; items
marked OWNER need a decision.

Severity: HIGH = would make a public claim of the repo false or a licence statement wrong; MEDIUM = should be
fixed or documented before publishing; LOW = hygiene.

## 0. Findings table

| # | Sev | File(s) | Finding | Action |
|---|---|---|---|---|
| F-01 | HIGH | `sys/dev/media/uapi/linux/media.h`, `sys/dev/media/uapi/README` | Labelled BSD-2-Clause and "NOT derived from Linux's media.h (GPL-2.0 only)". A line diff against Linux 6.12 `uapi/linux/media.h` shows 202 of 232 non-blank lines identical, in the same order and with the same tab alignment; what differs is the removed licence/copyright header, the removed comments, the removed ALSA/deprecated union members and a new header comment. This is a trimmed copy, not an independent expression. The statement in the file and README is not supportable. | RESOLVED 2026-10-08: option 2 done in a stronger form. `media.h` is now generated from `uapi/media.spec` by `tools/gen-media-h.py` (bare hex constants, own grouping and comments, ioctls through a local helper macro); 0 identical `#define` lines against Linux 6.12, the remaining identical lines are member declarations and struct openings forced by the ABI. Done by an AI session from a mechanical fact sheet, not a legal clean room. Owner may still ask counsel. |
| F-02 | MEDIUM | `uapi/linux/videodev2.h`, `v4l2-controls.h`, `v4l2-common.h` | Verified byte-identical to Linux 6.12 (no diff). Licence `(GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause`: the BSD-3 option is usable. Only `videodev2.h` carries the BSD-3 text in its body; `v4l2-controls.h` and `v4l2-common.h` carry only the SPDX tag, so a redistributor who sees those two files alone has no licence text. `Copyright (C) ... the contributors` and Nokia notices must stay. | Add a `sys/dev/media/uapi/LICENSES/` note or NOTICE with the BSD-3-Clause text (done in this audit: `uapi/NOTICE`). |
| F-03 | MEDIUM | `uapi/linux/dma-buf.h` | Ours, BSD-2-Clause, 22 lines: one struct, flag macros, one ioctl number (same as Linux's published uapi; 9 identical non-blank lines out of 22, all interface facts). Low copyrightability but the same family of risk as F-01. | Accept; mention in README. |
| F-04 | MEDIUM | `docs/cedrus-facts/ve-register-map.tsv` and `README.md` there | 337 register names (`MACC_*`) and descriptions "extracted from the wiki" (linux-sunxi). No attribution, no licence, no URL, no retrieval date in the file. The README says the wiki is "archived in `sources/`, CC BY" but **there is no `sources/` directory** in the repository or its history. I did not verify the wiki's actual licence (not guessed). The names include `{unsure}`/`???` wording copied from the wiki. | OWNER: Q2. Action taken: README wording corrected to say `sources/` is not in the repo; attribution header cannot be written without the URL/date. |
| F-05 | MEDIUM | `docs/cedrus-facts/README.md` | The author of the driver is stated to be "a language model that has seen public code, including GPL drivers for this very block, during training". Good that it is stated; it is in tension with the top-level README sentence "no Linux driver source was read or copied" (true as a statement about this project's inputs, silent about model training). | Top-level README clean-room paragraph now points to this limit. |
| F-06 | MEDIUM | `sys/dev/media/vbq_if.m`, `vm2m_if.m`, `vm2m.h`, `vm2m.c` | Method and function names mirror the Linux kernel framework: `queue_setup`, `buf_prepare`, `buf_queue`, `start_streaming`, `stop_streaming` (= `vb2_ops`), `device_run`, `job_ready`, `job_abort` (= `v4l2_m2m_ops`), `vm2m_buf_done_and_job_finish` (= `v4l2_m2m_buf_done_and_job_finish`), `fh_open`, `s_fmt`, `g_fmt`, `try_fmt`, `querycap`, `enum_fmt`. `vbq_if.m` says "follow Linux's vb2_ops on purpose (a Linux driver is the hardware reference)". These are the names userland/driver authors of the V4L2 ecosystem expect (API names, not code), but the comment shows the design was modelled on the Linux framework. I found no sign of copied bodies (no kernel-doc, no Linux idioms such as `container_of`, `devm_`, `regmap`, `kref`, `dev_err`). | Reworded `vbq_if.m` comment ("Linux driver is the hardware reference" is ambiguous). OWNER: Q3 whether to state "framework API modelled on Linux's vb2/m2m" in README. |
| F-07 | LOW | `sys/dev/cedrus/cedrus_dec.h` | All 67 hex-valued defines were checked: every offset or value appears in at least one facts file (`M0xx*.md`); 36 carry their own `F:Mxxx` tag, 31 are untagged siblings (see section 2). Names differ from the names used in the register-map TSV (`H264_PICSIZE` vs `MACC_H264_SEQ_HDR` and so on), so names were chosen by us. | none |
| F-08 | MEDIUM | `sys/dev/media/tools/**`, `docs/cedrus-facts/**` scripts | 13 of our code files under `sys/` and 45 under `docs/` (py, c, sh) had no SPDX tag. None carries third-party text (grep for derived/copied/adapted/GPL found nothing but two comments). | SPDX `BSD-2-Clause` added (separate commit). |
| F-09 | MEDIUM | `docs/cedrus-facts/measurements/M028-mono/*-input.bin` (3), `M031-sp/SP1_BT_A.12.264`, `M031-sp/sp2_bt_b.12.264` | These are excerpts of JVT conformance streams (names `HPCVMOLQ_BRCM_B`, `HPCAMOLQ_BRCM_B`, `FRExt_MMCO4_Sony_B`, `SP1_BT_A`, `sp2_bt_b`; contributors Broadcom, Sony, BT). 5 files, 6 KB to 21 KB each. The terms of the conformance suite (ITU-T H.264.1 / JVT-site zips) were not checked; the repo itself does not state them. Everything else of the corpus is external (`linux-work/jvt`, not committed). | OWNER: Q4. Either remove the five, or add a note naming the source and leave. |
| F-10 | LOW | `M032-switch/jm-*`, `M035-sp-minimal/`, `M0xx` JM files | Streams and traces produced by running the JM reference encoder/decoder (`encoder.txt`, `canon.txt`); JM source or binaries are not committed. JM has its own licence; output bitstreams generated by it are data. M037 notes "JM oracle ... `MAX_NUM_SLICES` raised from 50 to 512": a locally patched JM exists outside the repo. | none; mention only. |
| F-11 | LOW | `M036-cedarx/h264-elf.txt` (17 KB), `mmio.txt` files | `readelf` text of a vendor plugin (header, dynamic section, `.dynsym` of 174 entries) and logged register accesses by the vendor plugin. No code, no disassembly. No ELF object, `.so`, `.ko`, `.o`, `.a` or archive is tracked anywhere (checked with `file` over all tracked files). Vendor headers (`videoengine.h`, `vdecoder.h`, ...) are `#include`d by `cedar-probe.c` but are not in the repo (`fetch-sdk.py`, pinned blob ids in `sdk-provenance.json`). Confirmed: **no CedarX binary or vendor header is committed**. | none |
| F-12 | MEDIUM | `README.md`, `docs/cedrus-facts/HANDOVER.md`, `M040-speed-and-kms.md` | Over-claims (section 3). | Fixed (separate commit). |
| F-13 | LOW | `docs/cedrus-facts/HANDOVER.md`, `HANDOFF.md`, many facts | Refer to private paths (`linux-work/...`, `/opt/bzdos/...`) and to the old hashes. Not a licence matter, but a reviewer cannot follow them. | Mention in README of the audit; no edit. |
| F-14 | MEDIUM | Git history | Authorship carries no AI trailer; the history was rewritten on import ("committers normalised to Andrey Bodrov", `COMMIT-MAP.md`), 56 commits in four days. The history can neither confirm nor refute the README's AI statement. | See section 4. |
| F-15 | LOW | `sys/dev/media/shim/linux/*.h`, `tests/abi/linux-*-shim` | 4 + 6 tiny headers, all ours (BSD-2-Clause SPDX); the `asm/*.h` ones are one-line includes of `asm-generic`. Their names are dictated by the include paths the UAPI headers use. `shim/linux/compiler.h` equals `tests/abi/linux-asm-shim/linux/compiler.h` (duplicate). | none |
| F-16 | LOW | `tests/abi/gen_abi.py`, `tests/abi/README` | The test reads `/opt/bzdos/linux-work/linux-6.12` (GPL-only `media.h`, `v4l2-subdev.h`) at run time; nothing of it is copied into the repo. The test is what makes the F-01 equivalence visible. | none |

## 1. Licence inventory

Top level: `LICENSE` is BSD 2-Clause, "Copyright (c) 2026 Andrey Bodrov". 

Counts of SPDX tags in tracked sources (before this audit's fixes): 87 files `BSD-2-Clause` (58 in `/* */`
form, 29 in `#` form); 3 files `((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause)`; 58 `.c .h .m .py .sh` files
without a tag (13 in `sys/`, 45 in `docs/`, all scripts and measurement helpers of ours). After the fix commit every `.c .h .m .py
.sh` file has a tag (Markdown, JSON, `.txt`, `.lab`, `.264`, `.bin` data files carry none by nature).

### 1.1 The Linux UAPI headers (`sys/dev/media/uapi/linux`)

| File | Origin | Licence the text actually carries | Status |
|---|---|---|---|
| `videodev2.h` | verbatim from Linux 6.12 (`diff` empty) | SPDX `(GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause`; body: GPL-2+ notice, then "Alternatively you can redistribute this file under the terms of the BSD license as stated below" with a 3-clause BSD text | Dual. BSD-3-Clause option self-contained. |
| `v4l2-controls.h` | verbatim from Linux 6.12 (`diff` empty) | SPDX same; **no licence text in the body** (only "Copyright (C) 1999-2012 the contributors") | BSD-3 option rests on the SPDX tag alone: add the text (F-02). |
| `v4l2-common.h` | verbatim from Linux 6.12 (`diff` empty) | SPDX same; body "Copyright (C) 2012 Nokia Corporation, Contact: Sakari Ailus" | same |
| `media.h` | trimmed copy, see F-01 | SPDX claims BSD-2-Clause; upstream is `GPL-2.0 WITH Linux-syscall-note` (copyright Nokia, L. Pinchart, S. Ailus and others) | **unsupported claim** |
| `dma-buf.h` | ours, minimal | BSD-2-Clause | upstream is `GPL-2.0 WITH Linux-syscall-note`; same interface numbers (F-03) |

What the Linux-syscall-note allows: the exception text reads "NOTE! This copyright does *not* cover user programs
that use kernel services by normal system calls - this is merely considered normal use of the kernel, and does
*not* fall under the heading of 'derived work'." (this is the text of Linux's `LICENSES/exceptions/Linux-syscall-note`
as I know it; the file is not in this repo, so check the wording against the Linux tree before quoting). It tells
that a *program* using the syscall interface is not made a derived work by the header; it does **not** relicense the
header and does not speak of a FreeBSD kernel module that includes it. So for `media.h` the exception does not turn
a GPL-2.0 file into a BSD one.

Minimal clean options for `media.h` (owner chooses, see Q1):
1. Keep the file as upstream's text with upstream's SPDX (`GPL-2.0 WITH Linux-syscall-note`) and say in the README that the
   repo contains this one GPL-2.0 file, used by userland and by the media-controller core; simplest, honest, but the media
   module's source then includes a GPL-only header (the combination question belongs to the owner/counsel).
2. Rewrite `media.h` from the documentation of the interface (the kernel.org media-controller user-space API pages) by a
   person/session that does not look at the header, keeping only what the test `tests/abi` verifies (values, sizes,
   ioctl numbers); names are unavoidably the same, comments and layout must not be.
3. Drop the media controller device node from the public tree (the stateless decoders need it for requests, so this
   changes the product).

### 1.2 Other directories

* `sys/dev/{media,dmabuf,cedrus}/*.c *.h *.m`, `sys/modules/*/Makefile`, tests, tools: BSD-2-Clause by SPDX; the only
  other licence text anywhere in sys/ is in the UAPI headers above. No third-party copyright line other than in UAPI
  headers (`grep Copyright` over the tree: the LICENSE file and `ffmpeg version ... Copyright (c) 2000-2026 the FFmpeg
  developers` banners inside three captured `ffmpeg -i` outputs, which are tool output, not FFmpeg code).
* `sys/dev/media/shim/`: ours, tiny (F-15).
* `docs/cedrus-facts/ve-register-map.tsv`: third-party-derived data (F-04).
* `docs/cedrus-facts/**`: measurement scripts and logs of ours; captured Linux register traces
  (`linux-trace-*.txt`, `M035-sp-minimal/linux/trace.txt`, 0.17-0.6 MB each) are logs of hardware accesses made by an
  unmodified Linux guest, i.e. observations, not Linux source text.
* `docs/specs/SPEC_media_v4l2_native.md`: mentions GPL headers and decision Q1/Q3; not reproduced text.
* Quoting third-party material in docs: the FreeBSD policy sentences in section 4 (short quotes); the JVT/JM stream
  names; the vendor GitHub URLs (links only, `sdk-provenance.json`).

## 2. Clean-room provenance of the driver sources

Files read: `cedrus_dec.h` (whole), `cedrus_h264.c` (lines 40-56, 225-723), greps over all of `sys/dev/{cedrus,media,dmabuf}`.

Register defines (`cedrus_dec.h`, 67 hex-valued defines), checked mechanically against `docs/cedrus-facts/measurements/M*.md`:
* every offset / value occurs in at least one measurement file;
* for the 36 defines that cite `F:Mxxx`, the cited file contains the offset (verified for all but two, where the
  fact describes a bit rather than the value: `H264_STATUS_SKIPPED` 0x10000 and `H264_STATUS_VLD_BUSY` 0x100 cite M023,
  which says "bit 16" and "bit 8"; `MPEG_CTRL_JOB` cites M010 too, but the value is in M006);
* untagged defines (traceable only through their siblings): `MPEG_STATUS_FINISH/ERROR`, `MPEG_REC_CHROMA`,
  `MPEG_FWD_*`, `MPEG_BACK_*`, `VE_QUANT_INTRA`, `MPEG_ERR2`, `H264_FLAGS`, `H264_SLICE`, `H264_DBF`, `H264_QP`,
  `H264_CMD`, `H264_CMD_RESET/START`, `H264_STATUS`, `H264_STATUS_ERROR/FLAGS`, `H264_VLD_*`, `H264_VBV_END`,
  `H264_ENGINE`, `H264_CUR`, `H264_BUF_B`, `H264_X050`, `VE_SRAM_PTR/DATA`, `SRAM_H264_LIST1`, `SRAM_H264_SCALE4`.
  They are present in M014/M015/M023 and in the 0x0200..0x0254 formulas of `F:M014-c` (the slice registers); the
  bit layouts written in `cedrus_h264.c` (e.g. `H264_FLAGS` bit 15/10/5/4/2/1/0, `H264_SLICE` bits 24/16/12/8/4/3/2,
  `H264_DBF` `0x1000 |`, `H264_QP` bit 24) match the formulas in `F:M014-c`. The newer bits
  (`PICSIZE` bits 16-19, `H264_SLICE` bit 3/4 for fields, `0x100/0x200/0x11` in the DPB table words) trace to
  M026, M027-d, M028-a.
* Names: the tsv (from the wiki) uses `MACC_H264_SEQ_HDR`, `PIC_HDR`, `SLICE_HDR`, `PRED_WEIGHT`, `TRIG`,
  `VLD_END`...; the driver uses `H264_PICSIZE`, `FLAGS`, `SLICE`, `WDENOM`, `CMD`, `VBV_END`. MPEG names
  (`MPEG_PHDR`, `MPEG_SIZE`, `MPEG_FRAME_SIZE`, `MPEG_MBA`, `MPEG_CTRL`, `MPEG_TRIG`, `MPEG_STATUS`, `MPEG_VLD_*`,
  `MPEG_VBV_END`, `MPEG_REC/FWD/BACK_LUMA/CHROMA`) are the tsv's names without the `MACC_` prefix, so those come from
  the wiki, not from Linux (I did not look at Linux's names). I cannot say from here whether the Linux driver uses the
  same: any overlap is explained by the shared wiki ancestry, which must therefore be documented (F-04).
* Not traceable by a measurement file: nothing found in `cedrus_dec.h`. Names of C structures
  (`cedrus_ctx`, `cedrus_dec`, `cedrus_fs`, `cedrus_mem`): `cedrus_ctx` is a generic name; I cannot compare to Linux.

Programming sequences in `cedrus_h264.c`, `cedrus_dec.c`, `cedrus.c`: 46 `F:M` tags in 2224 lines. Every sequence step
that I read in `cedrus_h264_start()` carries an F tag at block level (table `F:M014-e`, formats `F:M014-b`,
scaling `F:M014-h`, skip `F:M014-d`/`M023`, weights `F:M014-g/M015-d/e`, lists `F:M014-f`, slice registers `F:M014-c`,
completion `F:M014-b step 10`). Individual constants without their own tag in that function:
`H264_X050 = 0` write (M005/M014 mention 0x050; its meaning is not documented), the `7` in `cedrus_vld_addr(…, 7)`,
`IRQEN = 7`, `coff / 4` (M010-e/c), the `0x100/0x200` mode words (M026), the clamp `i < 32`.
`cedrus_dec.c` (MPEG-2, 938 lines) and the lab (`ve_lab.c`, 222 lines, 0 tags: a generic poke/peek tool) I scanned only by grep.
I did not review `cedrus_dec.c`'s MPEG-2 sequences line by line; its 18 tags cover the register defines.

Linux-specific strings in driver/core code (grep `linux|sunxi|vb2_|v4l2_m2m|devm_|regmap|container_of|dev_err|pm_runtime|readl|writel|kref|vdpau|libva|ffmpeg|kwiboo`):
* comments that describe Linux *behaviour as observed* ("as Linux programs them (F:M017-a)", "Linux waits for the reader to
  go idle (F:M023)", "the Linux driver wrote 0x7fffffff there (F:M006)"): consistent with trace-based method;
* `vbq_if.m` header comment about vb2_ops (F-06); `vdev.c:680`, `vctrl.c:798`, `vbq.c:611`, `vbq.c:739` comments that say what
  Linux does for compatibility with `v4l2-compliance`: behaviour descriptions, not code;
* FFmpeg mentions in `cedrus_h264.c` explain the front-end conventions (`header_bit_size`, DPB numbering); no FFmpeg code.
* No Linux kernel-style helpers or macros were found in `sys/dev/cedrus`, `sys/dev/media/*.c`, `sys/dev/dmabuf`.
* The V4L2 stateless H.264 control structures come from the UAPI headers (BSD-3 option), as README says.

Things I cannot trace and a reviewer must decide:
1. Whether the sequence that the *Linux guest* executes was copied in form where that form is an arbitrary choice
   (order of register writes in steps 1-10 of M014-b): by construction the method replays it, which is observation of
   behaviour, not source.
2. The structure of the V4L2 core (F-06) and its relation to the Linux framework as learned by the model (F-05).
3. `ve-register-map.tsv` origin (F-04).
4. The names `M0xx`-era `F:` tags cite old file names in some comments (`F:M014-k`, ...), all of which exist.

## 3. Over-claims

Fixed in the commit "docs: bound the claims" (each fix cites the bounding fact):

| Where | Was | Now | Bound by |
|---|---|---|---|
| `README.md` table | "the exact UAPI headers" | "the UAPI headers" (one of five is not a verbatim copy, F-01) | section 1.1 |
| `README.md` status | "176 of 176 supported streams of the JVT conformance suite, 19,028 of 19,028 frames bit-exact on the board" | adds "of 231 streams; 55 are not supported" and that "supported" is the set the driver accepts, compared against JM/FFmpeg output | `HANDOVER.md` section 7 (176/231, 55 refusals: 27 ten-bit, 13 fourteen-bit, ten 4:2:2, three FMO, two SP) |
| `README.md` status | "Not supported by the hardware as measured" | "not working in our measurements (M029-M037); not proven to be silicon limits" | `HANDOVER.md` (M029 "formats1" is "a Linux driver refusal, not a proven hardware limitation") |
| `README.md` clean room | "no Linux driver source was read or copied" | same sentence plus the limits of the method (model training, `ve-register-map.tsv` from a wiki, F-01) | `cedrus-facts/README.md` "Limits of this scheme" |
| `HANDOVER.md` section "What the decoder does today" | "MPEG-2 I/P/B frames ... exact" | "within +-2 per sample" | M038-a |
| `M040-speed-and-kms.md` | "the H.264 decode itself is bit-exact (M039)" | adds the bound: first 30 packets of each stream, 173 supported streams | M039-c |
| `M039-ffmpeg-v4l2-request.md` F:M039-a/c | "39 of 39 exact", "173 of 173 byte-identical" | already bounded in the text (30 packets each; MPEG-2 +-2); left | n/a |

Not changed but worth knowing: `HANDOVER.md` section 7: "production acceptance remains 176/231"; 174/174 and 176/176 are
different runs (M027 vs M028) of the *supported* subset; in the M026 table there were 3 mismatching streams before M027-d fixed
them. `HANDOFF.md:89` "decodes bit-exactly on the board at 176x144 ... 1280x720" is listed with the streams it names. The
corpus sweep through FFmpeg (M039-c) covers only the **first 30 packets** of each of 173 streams (a bound, not a defect).
MPEG-2 evidence: 11 streams within +-2; one 60-picture clip drifts further (M038-a: 24 of 60 pictures within +-2,
36 with a few samples further off); README already says so.

## 4. AI-assistance statement

* README: "The code was written with AI assistance and reviewed by the owner." The facts README is stricter: "the author
  of the driver is a language model that has seen public code, including GPL drivers for this very block, during training."
* Git history: 56 commits, author and committer `Andrey Bodrov <ap.bodrov@gmail.com>`, no `Co-Authored-By`, no
  "generated" trailer (grep over all messages: none). This follows the owner's stated commit convention; it means the
  history **neither supports nor contradicts** the README. The history was rebuilt on 2026-10-08
  (`COMMIT-MAP.md`: "committers normalised"), so original metadata is not preserved here. The volume (56 commits, about
  4 days) is consistent with the README but does not prove it. I did not inspect the old session transcripts.
* "Reviewed by the owner": not verifiable from the repository (no review records). Not guessed.
* FreeBSD policy, fetched 2026-10-08 with the proxy unset:
  * `https://raw.githubusercontent.com/freebsd/freebsd-src/main/CONTRIBUTING.md` (main): under "Do NOT submit a Pull Request for":
    "Changes generated by AI tools without substantial human review and validation." and under quality:
    "Automated accounts or chatbots must not submit pull requests or use pull request interactions to train AI or LLM systems."
  * `.../freebsd-src/releng/15.1/CONTRIBUTING.md`: "Submissions using generative AI will be rejected." and
    "Submissions from AI chatbots will result in the account being banned."
  * The Handbook contributing article (`https://docs.freebsd.org/en/articles/contributing/`) has no AI wording in the text I searched.
  Consequence: the README's sentence "FreeBSD's own policy on such contributions applies to anything proposed upstream" is
  accurate and needs no change; but it should name the branch dependence (main vs releng/15.1 differ). The owner's
  project memory records the same difference. Whether the work meets "substantial human review and validation" is for the
  owner to attest; the repository does not contain the evidence either way.
* Not verified: the policy at the time of any future submission; whether a separate project policy exists
  (e.g. on the mailing lists or in `freebsd-doc` outside CONTRIBUTING); the legal status of AI-assisted output
  (copyright of the generated code, training-data exposure).

## 5. Third-party material

| Item | Where | State |
|---|---|---|
| Kwiboo's FFmpeg `v4l2-request-n8.1` branch (LGPL-2.1+/GPL as built) | referenced in README, M039, `sys/dev/media/tools/ffmpeg/build-ffmpeg.sh` | not in the repo; the script fetches/builds it. `ffmpeg-test.sh`, `ffkms.c`, `cmpyuv.c` are ours. Using FFmpeg as a separate program is no licence issue for BSD-2 files; **distributing a built `ffmpeg` binary would be LGPL/GPL**, none is committed. The branch's own licence and URL are not recorded in the repo: add them to `build-ffmpeg.sh` header (Q5). |
| `libudev-shim.c` | `sys/dev/media/tools/ffmpeg/` | ours (150 lines, SPDX BSD-2). |
| `mi-sysfs-shim.c` | `sys/dev/media/tools/` | ours. |
| JVT conformance streams | not committed; five excerpts, see F-09 | OWNER Q4. |
| JM reference software | not committed; outputs only (F-10) | fine. |
| Vendor CedarX plugins (3x `libawh264.so`) and headers | not committed; pinned by blob id and SHA256 in `M036-cedarx/sdk-provenance.json` | **confirmed none committed** (`file` over all tracked files: no ELF; names `*.so *.ko *.o *.a` absent). The 17 KB `h264-elf.txt` is `readelf` output. |
| Linux 6.12 UAPI headers | `sys/dev/media/uapi` | section 1.1 |
| linux-sunxi wiki register table | `ve-register-map.tsv` | F-04 |
| Linux register traces | `vectors/**/linux-trace-*`, `M035/linux/trace.txt` | observations of an unmodified Linux guest; fine. |

Largest tracked files (bytes): `M037-fmo-one-mb/{raster,fmo}/probe.lab` 1,287,348 each (replay scripts, text);
`M026-field-lab/weighted-trace.txt` 801,216; `longterm-trace.txt` 697,216; `vectors/h264/linux-trace-h264-set1/2/3.txt` 422,912 / 595,424 /
254,112; four `M032-switch/*/stream.264` 305,748 each; `M030-pcm/pcm3/{stream.264,input.bin}` 304,950 / 304,928;
`M034-depth/14/stream.264` 266,934; `M028-mono/gray-weighted-headers.txt` 240,445; `M026-field-lab/corpus-final.json` 229,046.
Binary payloads in total: 129 files (`.264 .bin .m2v .yuv`) = 6.4 MB uncompressed. `.git` at this commit: pack 2.66 MiB +
loose 448 KiB (the "7.9 MB" figure was the pre-gc size). All the generated streams (M029-M037, `vectors/h264/streams/*.264`, `.m2v`)
are produced by our tools or by JM/x264/FFmpeg encoders run on synthetic sources; licence of such outputs is not
restricted by the encoders' code licences, but the source video of the older `vectors` is not documented (Q6).

## 6. Open questions for the owner

* Q1. `media.h`: answered by the regeneration of 2026-10-08 (F-01); the owner may still want counsel's view.
* Q2. DONE 2026-10-08 (owner delegated): the tsv was removed from the tree (history keeps it). Was: give the wiki URL/revision/licence so an attribution header can be written, or restore `sources/`; or drop the tsv from the public tree and rely on the measurements.
* Q3. DONE: README "Provenance notes" says so. Was: do you want the README to say that the V4L2 core's method names follow Linux's vb2/m2m API (F-06)?
* Q4. DONE: the five excerpts were removed; README says so.
* Q5. DONE (comment in build-ffmpeg.sh). Was: record the licence and URL of Kwiboo's FFmpeg branch in `build-ffmpeg.sh`.
* Q6. ANSWERED in `docs/cedrus-facts/vectors/PROVENANCE.md` (everything is generated from lavfi test patterns by libx264/mpeg2video/JM, no third-party footage); the one gap, the exact command for `play.m2v`, is not recorded anywhere and is stated as such. Was: source of the video content used by the oldest vectors (`flat.m2v`, `grad.m2v`, `i64.m2v` are synthetic by name; `play.m2v` and the early H.264 streams `f1.264 ... wp.264`: encoder and source not stated).
* Q7. DONE 2026-10-09: the sentence below is now in the README. Was: draft sentence for the owner to adopt or change (only the owner can attest the review): "This repository was written between 2026-07 and 2026-10 with AI assistance (language models) under the direction of Andrey Bodrov, who reviewed the design, ran the code on the board and takes responsibility for it. Register facts come from measurements of an unmodified Linux 6.12 guest and experiments on the board; the models used have seen public code in training." Was: AI statement: add a dated sentence naming which tools and that the owner reviewed (only you can attest); mention the branch dependence of FreeBSD's policy.
* Q8. DONE (README). Q6 and Q7 stay OPEN: only the owner can state the origin of the oldest vectors and attest the review. Was: rewriting history was done on import; if you publish, say so in README (COMMIT-MAP already does).
