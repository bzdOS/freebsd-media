# M029 — bounded higher-chroma probes and remaining format contracts

2026-10-08. M028's native driver remains unchanged (`fbsd-media 3cd9cd7`).
This measurement tests one candidate for extending chroma programming; it does
not claim a physical impossibility theorem for the A64 video engine.

## F:M029-a — extending PICSIZE alone is insufficient

Six local fixtures contain one 352x288 intra picture each: eight-bit 4:2:0,
4:2:2 and 4:4:4, each with CAVLC or CABAC. The same front end exports the slice
and controls. All use one slice, no references, no field pictures, no 8x8 transform
and no custom scaling lists. The positive controls eliminate a broken exporter,
header-skip sequence or enlarged-buffer layout as a blanket explanation.

The candidate places chroma_format_idc in PICSIZE bits 19..20, extending M028's
measured bit 19. **Bit 20 is a hypothesis, not a measured format definition.**
Within each higher-chroma A/B, the sole changed command is the PICSIZE write:
4:2:0 setting `0xd1511` versus candidate `0x151511` (422) or `0x1d1511` (444).
No output-format register, address, stride, input byte or other control changes.
The production driver does not contain this extension.

Output space is enlarged to **1MiB** at 0x200000, pre-filled with 0x5a, with
4KiB guards immediately before and after. MV storage is moved to 0x400000,
with guards at 0x3ff000 and 0x408000. The whole experiment remains within the
existing 8MiB lab allocation. Both existing scratch pointers remain unchanged.
Full planar references retain their original chroma resolution; comparison
interleaves U/V losslessly, never converting 422/444 to 420.

| Input | 420 setting | Candidate chroma setting |
|---|---|---|
| 420 CAVLC | exact full 152,064-byte NV12 | same positive control |
| 420 CABAC | exact full 152,064-byte NV12 | same positive control |
| 422 CAVLC | error, no output writes observed | error, no output writes observed |
| 422 CABAC | completion, wrong Y and UV | completion, wrong Y and UV |
| 444 CAVLC | error, no output writes observed | error, no output writes observed |
| 444 CABAC | completion, wrong Y and UV | completion, wrong Y and UV |

All four guards remain intact in **all ten executions**. For 422 CABAC the last
changed output byte is at extent 122,304, against 202,752 bytes required; for 444
CABAC it is 113,888 against 304,128. An extent is only the highest byte differing
from the initial 0x5a, not a hardware write counter. CAVLC has status low bits 3;
CABAC has low bits 1 despite wrong pixels. The candidate changes some bytes but
fixes neither the Y plane nor full-color output.
[All results](M029-formats/results.json), exact scripts/inputs/logs in adjacent
subdirectories. [Reproduction and provenance](M029-formats/README.md).

This falsifies **sufficiency of that one programming change** on these fixtures.
It does not distinguish an unsupported hardware format from missing entropy,
transform, output-mode or other configuration. Changing output layout alone
would also be insufficient evidence: the current experiment's luma is already
wrong. Do not enable 422/444 in the native driver from completion status.

## F:M029-b — ordinary decoding recovers without a reboot

After all negative probes, the unchanged native driver completes f3 **8/8**,
weighted gray **36/36**, and gray MBAFF/CABAC **36/36**, full Y/C exact:
[recovery](M029-formats/driver-recovery.json). The final snapshot reports
**25,206 successful native slice/field jobs, errors=0, timeouts=0**, vbq zero live
allocations, cedrus_dec one 1KiB allocation. Lab operations bypass these native
job/error counters; their deliberate failures are recorded in their own logs.
[State and binary hashes](M029-formats/final-state.txt).

No guest reboot, image replacement or TFTP switch was needed for M027–M029.
The FreeBSD module remains `d57627e64bb4cfc21e64d264e9a19b5a`, exactly the M028
full-regression build. Lab scripts and all uploaded inputs were verified.

## Remaining explicit boundaries, not silently omitted scenarios

The complete 231-stream inventory is [M028's matrix](M028-mono/corpus-final.json):
**176 exact, zero pixel mismatches in the accepted set, 55 explicit parser refusals**.
The remaining work cannot be represented by simply relaxing the checks:

| Refused group | Streams | Missing implementation/measurement |
|---|---:|---|
| Higher bit depth | 40 (27 ten-bit, 13 fourteen-bit) | Entropy/reconstruction bit-depth programming and a full-depth capture format, stride/packing and buffer sizing. Current capture is exclusively eight-bit NV12. No high-depth lab capability is established. |
| Eight-bit 422/444 | 10 | Working entropy/reconstruction programming, then output format/stride/plane-size contract. M029 disproves the simple PICSIZE-only candidate. |
| FMO | 3 | Frontend group-map construction/transport and measured engine macroblock traversal. Current parser stops on nonzero num_slice_groups_minus1; no map is produced. |
| SP/SI | 2 | Switching-slice controls and measured switching reconstruction. Current frontend/driver refuse these slice types. |

High depth takes precedence over chroma when counting: the groups are disjoint.
The native driver separately rejects these unsupported control combinations
before programming a job (`cedrus_h264_prepare`). Direct control-injection checks
are described below; the full 55 corpus entries were not all forced past the parser.

Allwinner's [A64 product brief](https://device.report/m/5e508da69db1cd6c2a3c4cbae3a717174fe4530c25aea2369673fcbbd39f098e)
(advertised feature list, v1.0, 2015-03-23) names H.264 BP/MP/HP. It does not
supply register programming for these remaining formats or establish that every
syntax feature of those profiles works. Absence from a feature list is not a
hardware impossibility proof, just as M026's Linux mono rejection was not one.

The campaign has measured/classified every corpus entry and completed the
supported-stream regression. **Full support for the 55 refused entries is not
implemented or claimed.** Further capability work needs a concrete hardware
programming hypothesis or specification for the missing contract above; retaining
explicit refusal avoids exposing unsupported formats as successfully decoded.

## Direct driver refusal checks

A research-only copy of the existing cedrus_h264 test changes one control before
S_EXT_CTRLS, while feeding the original valid first f3 slice. This exercises the
kernel gate independently of the front-end format refusal. It does not establish
that the deliberately inconsistent controls describe valid 422/444/etc bitstreams.

The initial assay expected `errors` to stay constant and stopped after 422. Source
inspection of `cedrus_job_done` shows that this counter includes **all jobs returned
with an error**, including pre-engine rejections. The observed increments were
rejects +1, errors +1, pictures +0, timeouts +0, and both queues returned normally.
The acceptance criterion was corrected to those semantics; no driver was changed.
The initial failed assertion remains in reject-initial-assertion.json for provenance.

All **nine checks pass**: 422, 444, ten-bit, fourteen-bit, FMO, SP, SI,
separate-color-plane flag and nonzero plane id. Each request completes with
ERROR buffers, increments rejects/errors by one, and leaves pictures/timeouts
unchanged. [Results](M029-formats/reject-results.json),
[injection patch](M029-formats/reject-probe.patch), test source and runner adjacent.
The same instrumented binary without injection decodes the first picture exactly;
the unmodified native test then completes f3 **8/8**. Final native counters are
**pictures=25,215, rejects=9, errors=9, timeouts=0**. Those nine errors are the
nine deliberate software rejections, not failures of supported-stream decoding.
`vbq` returns to zero live allocations. [Final state](M029-formats/post-rejection-state.txt).
