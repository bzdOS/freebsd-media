# M033 — FMO requires more than a slice start address

2026-10-08. Two locally generated352x288,8-bit420 PCM pictures contain identical
prescribed pixels and two I slices. The positive control divides the raster into
two contiguous198-macroblock halves. The FMO input has two interleaved groups,
map_type0, run_length_minus1=0 for each group, visiting even and odd macroblocks
respectively. Each stream is independently decoded **exactly by JM19**.
[Generator](M033-fmo/fixtures.py), [validation](M033-fmo/validation.json).

The guarded lab uses the same measured I-slice register sequence for both,
adapting each slice's exact input size, header length and start position. No
group-map register or map buffer is supplied: none is established for A64 by
this project. The hypothesis is deliberately limited to whether the existing
slice-start mechanism alone could account for traversal.

| Input | Engine completion | Full152064-byte NV12 |
|---|---|---|
| Contiguous two-slice control | both slices done | exact |
| Interleaved FMO | both slices done | wrong,149047 bytes differ |

All **eight guard observations** across the two executions remain intact.
The first positive-control attempt timed out during SCP before any lab command
ran; only that incomplete upload was resumed, then verified before execution.
The FMO negative is not a transport failure or an error-flag-only conclusion.
[Results](M033-fmo/results.json), per-case replay commands and MD5 records.

This rules out silently ignoring the group map while merely setting first_mb.
It does not establish that no unpublished traversal mechanism exists. General
FMO support needs a measured way to supply or control macroblock traversal,
plus the corresponding frontend/group-map transport. Native FMO rejection
remains unchanged. No software reconstruction or image reordering was deployed.
Raw outputs are retained in `linux-work/h264-traces/m033-fmo` and identified by
[SHA256](M033-fmo/raw-sha256.json). Native recovery is recorded in M034.
