# M028 raw evidence

See ../M028-monochrome.md. These are executed scripts and saved observations,
not a standalone board deployment package. The matching host scratch layout is
`linux-work/h264-traces/m027-mono/NAME/{input.bin,params.json,chroma-N.lab,run-N/}`.
`prepare.py` and `run.py` are preserved research helpers; their relative paths
expect that original layout, the local JVT catalog, and the M027 replay baseline.

To replay an archived lab case, put `NAME-input.bin` in an otherwise empty guest
lab directory as `input.bin`, alongside the selected `NAME-chroma-N.lab`. Verify
both uploads against host MD5, acquire the shared board lock and run the existing
`ve_lab` with the script. Console capture must already be running. The saved
`*-upload-md5.json` gives the exact executed inputs. Compare the entire dumped
NV12 frame against the decoded reference; do not use the status bit alone.
The colored positive control is FRExt_MMCO4_Sony_B/chroma-1. The mono probes use
chroma-0; chroma-1 are intentional negative controls. The -uv5a script changes
only initial UV contents and proves that the engine writes neutral chroma.

`gray-weighted.264` is a local generated fixture, not an official corpus stream.
Generate its reference with ffmpeg to yuv420p, no cropping, all frames. It was
encoded from `testsrc2=size=176x144:rate=25,fade=t=in:st=0:d=1.2,format=gray`,
36 frames, libx264, two threads, `weightp=2:keyint=36:min-keyint=36:scenecut=0:bframes=2`.
`gray-color-gray.264` concatenates it, the existing eight-frame f3.264, then it
again; it exercises SPS format switches in one decoder context. Reference MD5s
and verified inputs are in custom-driver-results.json. Logs preserve every PASS.

`gray-mbaff-cabac{0,1}.264` add 36-frame monochrome MBAFF fixtures with each
entropy mode. Source filter: `testsrc2=size=352x288:rate=50,interlace=scan=tff,format=gray`;
x264: `tff=1:cabac=N:keyint=36:min-keyint=36:scenecut=0:bframes=2`.
Both parse as 36 MBAFF pictures and pass all 36 full Y/C comparisons.
This establishes these fixtures, not every possible mono/field combination.
