# M034 — valid higher-depth PCM through the known eight-bit path

2026-10-08. Three352x288420 I_PCM inputs contain prescribed8-,10- and14-bit
samples, including nonzero low sample bits in the higher-depth cases. Both JM19
and FFmpeg decode all three exactly to the generated references. The higher-depth
reference files use planar16-bit little-endian samples without reducing precision.
[Generator](M034-depth/fixtures.py), [validation](M034-depth/validation.json).

The guarded lab deliberately retains the **known eight-bit register mode** and
adjusts only the exact slice length/header skip. No high-depth mode bit or output
packing contract is known or introduced. This measures what happens when valid
high-depth payload reaches the established path rather than stopping at SPS
validation; it is not a test of a documented high-depth hardware configuration.

| Input | Result | Position | Raw error |
|---|---|---|---|
| 8 bit | exact152064-byte NV12 |0x1511 |0x40, normal control value |
| 10 bit | error, no changed output bytes |0 |0x42 |
| 14 bit | error, no changed output bytes |0x100 |0x42 |

All **12 guard observations** remain intact. The higher-depth comparison in
the raw reports is only a candidate16-bit interleaved layout, not an established
A64 capture format. The decisive negative is the engine error and lack of output,
not the speculative layout comparison. [Results](M034-depth/results.json).

These observations do not prove every possible high-depth configuration
impossible. M030 separately records the maintainer's report of a ten-bit H.264
hardware limitation. The published [A64 User Manual v1.1](https://wiki.friendlyelec.com/wiki/images/b/b4/Allwinner_A64_User_Manual_V1.1.pdf),
sections2.1.6.1–2, lists video capabilities but supplies no switching, FMO or
higher-depth H.264 programming sequence used by these experiments.

## Native recovery after M032–M034

The series contains **46 lab executions and141 intact guard observations**,
plus the new Linux trace cycle. After all probes, the unchanged native driver
passes f3 **8/8**, weighted mono **36/36**, and mono MBAFF/CABAC **36/36**, all
full Y/C exact. Uploads were MD5-verified. Fresh-boot final counters are
pictures80, errors0, timeouts0, rejects0; vbq live0; cedrus_dec one1KiB allocation.
The enabled lab retains its intentional8MiB allocation. Lab failures bypass
native job counters and must be read from their own reports.
[Recovery](M034-depth/recovery.json), [state](M034-depth/final-state.txt).

Production module MD5 remains d57627e64bb4cfc21e64d264e9a19b5a. FreeBSD TFTP,
SSH and UFS are restored. Console follower PID3135592 writes
`linux-work/h264-traces/m032-switch/console.txt`; verify its current command
before stopping it for another exclusive debug-channel cycle. Nothing pushed.

The measured production capability remains M028's176 accepted corpus streams,
with the M030 monochrome-PCM caveat. General SP/SI, FMO, higher chroma/depth and
mono PCM are not implemented by these measurements. All enumerated experiments
are finished; the full hardware-only feature request is **not complete**.
