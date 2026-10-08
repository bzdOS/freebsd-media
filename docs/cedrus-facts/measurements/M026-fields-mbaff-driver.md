# M026 — fields/MBAFF replay, MV bounds and FreeBSD implementation

The residual long-term pixel failures and repeated-corpus allocation acceptance
are superseded by [M027](M027-dma-longterm.md): all 174 accepted streams now match.
The measurements below preserve the earlier observations and failed attempts.

2026-10-08. Clean-room implementation: measured Linux MMIO/SRAM traces and
published V4L2 UAPI, no Linux decoder implementation source.

## F:M026-a — complete NV12 replay and a real MV overwrite

Exporting request slices with shared timestamps for field pairs maps all 20
M024 runs to the exact front-end inputs. Relocated FreeBSD lab replay matches
all **9/9 interlaced frames**, including chroma: 2 CVPA, 4 CAMA, 3 CAPAMA.
See [PAFF](M026-field-lab/paff-ack-mv2.json),
[MBAFF](M026-field-lab/mbaff-ack-mv2.json),
[mixed](M026-field-lab/mixed-ack-mv2.json) and adjacent console/control files.

The progressive allocation formula, rounded to 16KiB at 352x288, is unsafe for
fields: the PAFF replay modifies 1,760 bytes of the following 4KiB canary, as far
as byte 2,239. Both output frames nevertheless match. Repeated with proper
completion acknowledgement: [small MV](M026-field-lab/paff-ack-mv1.json).
Doubling the per-MB MV allowance gives a 28KiB allocation and intact canaries
on all three interlaced clips. The measured half spacing remains 12,672 bytes.

M025's original arithmetic was wrong and has been corrected: interlaced half
spacing is `width_in_mbs * full_frame_height_in_mbs * 32`, versus `*16` for
progressive with direct_8x8_inference=1. It is **not** the same formula for both. Allocation keeps extra
row/padding space; these measurements do not prove an exact minimal bound.

The new replay initially inherited the old helper's omission of status W1C
acknowledgement, but omitted its 20ms sleep. The stale completion bit caused
premature dumps and nondeterministic mismatches. Replaying the actual W1C
writes fixes this: only the `*-ack-*` runs above establish decode results.

## F:M026-b — SRAM modes are necessary

Clearing just word 2 of every table entry in the mixed replay makes **0/3**
frames exact, with intact canaries:
[negative control](M026-field-lab/mixed-clear-mode.json).
Baseline modes are 0x100 for field pictures, 0x200 for MBAFF frames and zero
for progressive frames. Reference entries retain their own coding mode.
Reference-list bytes use twice the DPB slot plus one for a bottom-field reference.

## F:M026-c — driver and test

The driver accepts 8-bit 4:2:0 fields/MBAFF, programs measured picture/slice
flags, uses the field selector in reference lists, retains the current slot
when the second field names the first, and doubles the interlaced MV allowance.
The capture timestamp is assigned before resolving the current field's references.
The test holds one capture buffer across both fields and compares only complete
frames in display order. A released capture is dequeued even on a held-slice error.

Initial driver test: **9/9 interlaced frames + 8/8 progressive controls**, full
Y/C byte comparison. Logs: `driver-s0.txt`, `driver-s1.txt`, `driver-s2.txt`,
`driver-f3.txt` in [artifacts](M026-field-lab/).

## F:M026-d — inherited SPS scaling matrices

Corpus testing exposed a front-end bug independent of field register programming:
when a scaling matrix came from SPS, `has_sm` was true but the PPS UAPI flag
`V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT` remained clear. The published UAPI
explicitly defines this as a picture-level flag, including inherited matrices.
Setting it fixes all six targeted failing clips: FREXT01_JVC_D, FREXT02_JVC_C,
FRExt2_Panasonic_C, FRExt4_Panasonic_B, HCAFF1_HHI_B, HCAMFF1_HHI_B.

## F:M026-e — direct_8x8_inference changes progressive MV geometry

The `weighted2` Linux run preserved 25,038 entries (`isv0=0`), 117 slice runs:
[trace](M026-field-lab/weighted-trace.txt), [console](M026-field-lab/weighted-console.txt).
The first six streams completed; the last, BASQP1, hit an engine error on its
13th slice and the rig blocked dequeuing a held capture. No TRACE-DONE occurred;
the cycle is explicitly a **partial capture**, not a successful whole run.
FreeBSD SSH/UFS and its TFTP pairing were restored.

At 352x288, progressive direct8=0 uses **12,672 bytes** between MV halves,
twice the compact direct8=1 distance of 6,336. The previous FreeBSD code used
the compact layout unconditionally for progressive pictures. Both the spacing
and allocation now grow by two for direct8=0. Completed Linux luma checks:
CVWP2 12/12, CVWP3 12/12, HCAFR2 10/10, FRExt3_Panasonic 11/11.
The corrected FreeBSD driver matches **90/90 + 90/90 + 10/10 + 11/11** full
Y/UV frames: [results](M026-field-lab/direct8-fixed.json).
CVWP2 contains 58 real explicit-weighted B slices; this covers the actual
bitstream case, not only a forced-control variant.

Linux reused capture/MV storage between equal-sized streams: the following
direct8=1 MMCO stream initially retained the larger spacing on already-used
buffers. The rig now releases mappings and buffers between *every* stream.
This observation is not evidence that direct8=1 requires the larger spacing.

## F:M026-f — emulation-prevention bytes are not VLD skip bits

BASQP1's first picture failed at first_mb=60, on both FreeBSD and Linux.
The slice header contains an emulation-prevention 0x03. The front end added
eight bits for that byte although the engine removes it while reading.
Changing only that header count from escaped bits to RBSP bits makes all
**4/4 frames exact**, including all 80 slices:
[result](M026-field-lab/epb-fixed.json), [log](M026-field-lab/BASQP1_Sony_C.txt).
Start-code and NAL-header bits remain counted. The parser's obsolete escaped
offset conversion and EPB index bookkeeping were removed.

## F:M026-g — corpus assay corrections

The test now keeps both fields in one capture, checks errors before dequeuing
held captures, and does not confuse unused capture timestamp zero with a
non-existing DPB picture. The full reference is mapped from tmpfs rather than
copied into another allocation. HCHP3 previously obtained just five capture
buffers and stopped; it now matches **250/250** frames.

The host assay derives raw dimensions from decoded frames, not cropped stream
metadata (CVFC1). `-fps_mode passthrough` prevents synthetic duplicated reference
frames in CVSE2, CVSE3 and CVSEFDFT3. Entire streams are compared, with explicit
expected frame counts and MD5-verified uploads; absence of an error is not a pass.

## F:M026-h — remaining long-term cases, not a FreeBSD-only discrepancy

Against official YUV, the same front end on Linux differs at MR9 decoded frames
26 and 27 (among the first 32), and MMCO4 decoded frame 23 (among the first 24).
These are also the FreeBSD failures. See
[comparison](M026-field-lab/longterm-comparison.json).
The full FreeBSD streams currently match 49/58 and 59/60 respectively.
MR8_BT_B adds another long-term field/B case: 52/58 complete frames match,
with six pixel mismatches (not allocation failures). BA3, MR6 and MR7 match
33/33, 60/60 and 60/60: [Extended-profile results](M026-field-lab/extended-profile.json).
Retaining unmarked reference metadata in the front end changes neither result
([negative experiment](M026-field-lab/retain-negative.json)); that experiment
was not incorporated. Agreement between drivers does not identify the cause:
front-end controls and shared engine/driver limitations remain possibilities.

The strict front end was run on all 231 corpus files:
[inventory](M026-field-lab/parser-scope.json). 174 parse completely, 57 return
unsupported syntax/format (-2). Four accepted streams had been unnecessarily
filtered by profile number: BA3_SVA_C, MR6_BT_B, MR7_BT_B and MR8_BT_B.
The board assay now selects syntax/format rather than rejecting Extended
Profile wholesale. Full-corpus regression, including those four, is accounted for below.
No implementation claim is made for 4:2:2, 10-bit, monochrome, FMO or SP/SI.

## F:M026-i — direct8 canaries and Linux format refusals

Relocated replay of CVWP2's first 12 pictures matches **12/12 complete NV12
frames**, with all twelve 4KiB MV guards intact and successful final engine
status for every slice: [comparison](M026-field-lab/direct8-canaries.json).
This validates the enlarged direct8=0 allocation independently of pixel output.

The `formats1` cycle captured 3,598 valid entries. BASQP1's corrected first
picture now completes all 20 slices and its luma matches ffmpeg on Linux too:
[comparison](M026-field-lab/formats-epb-comparison.json).
The following five first-picture probes all return EINVAL from SPS control
validation (`error_idx=0`): HPCVMOLQ_BRCM_B (monochrome), Hi422FR1_SONY_A and
Hi422FREXT16_SONY_A (4:2:2), PPH10I1_Panasonic_A and FREH10-1 (10-bit).
See [results](M026-field-lab/formats-format-results.json) and the adjacent
console/trace/manifest. These are measured Linux driver refusals, **not proof
of hardware inability**. They provide no programming trace for those formats.


## F:M026-j — complete corpus accounting and allocation failures

[Final matrix](M026-field-lab/corpus-final.json) classifies all **231** streams:
**171 complete streams exact**, **3 with pixel mismatches** (MR8, MR9, MMCO4),
and **57 explicitly outside the strict parser scope**. The accepted streams
contain 18828 frames; 18812 compare exactly in full Y/C. This is not an all-pass result.
Earlier failed attempts remain in the matrix; success was not inferred from
absence of an error message.

After the long sequential run, eleven streams stopped at REQBUFS ENOMEM or
received too few capture buffers. All eleven complete exactly after reboot
with the same module and test. MR8's six pixel failures remain unchanged on
the fresh guest. The saved memory snapshot has 307,869 free pages and
`cedrus_dec` back to one 1KiB allocation, while DMA is constrained to the first
256MiB. This supports constrained contiguous-memory fragmentation, but does
not prove its root cause or fix long-duration allocation reliability.
Before reboot, driver counters were 24,022 pictures, zero engine errors,
zero timeouts and zero missing references. Those counters do not certify pixels.

## F:M026-k — long-term flag black-box A/B

`longterm1` captured 21,788 valid entries, 104 slices. Linux's first 28 MR8
frames match 27/28; decoded frame 27 fails, as on FreeBSD. MMCO4 again matches
23/24. [Comparison](M026-field-lab/longterm1-comparison.json).

Two fresh-buffer MMCO4 runs differ in exactly one supplied control: variant 5
clears DPB LONG_TERM flags, leaving the stream, lists, POCs and all other
controls untouched. **All 5,612 expanded VE writes are identical**, including
addresses, tables and list words; all PIC records are identical too:
[A/B result](M026-field-lab/longterm-longterm-flag-ab.json).
This establishes that the tested Linux path does not change VE programming
for that flag on this clip. It does not prove that ignoring the flag causes
all three remaining failures, nor reveal a hardware encoding to implement.
No speculative register bits were added to the FreeBSD driver.

Next investigation: independently validate reference lists and colocated
reference identity at the first failing B pictures. A programming fix for
long-term distinctions needs a measured working oracle or published hardware
contract; the failing Linux path alone cannot supply one.

## Final repository and board state

Implementation/toolbox: `fbsd-media` commit **1860bca**, not pushed.
`make sema-check` passes. The final guest is FreeBSD, UFS `/var` mounted;
modules, test and lab are installed, all 18 uploaded inputs MD5-verified.
Cedrus module MD5: `09fc56a7b52bbe6c5c633b11643fa1b0`.
A deliberately truncated frame returns ERROR as expected; a following f3 run
is **8/8 full Y/C exact**, no timeout. Final `errors=1` is that intentional
negative test, not a new regression. See `truncated.txt`, `final-f3.txt`,
`guest-final.txt` and `guest-input-md5.json` in the artifacts directory.
