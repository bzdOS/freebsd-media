# M029 bounded format probes

See ../M029-format-boundary.md. `stream.264` fixtures are generated locally with
ffmpeg/libx264: one 352x288 I-frame from
`testsrc2=size=352x288:rate=25,format=FORMAT`, output pix_fmt=FORMAT,
two encoder threads, `cabac=N:keyint=1:8x8dct=0`. FORMAT is yuv420p/yuv422p/yuv444p;
N=0/1. `params.json` and `input.bin` were exported by M028-mono/export.c using
the same BSD h264ctl.h. The full reference is ffmpeg rawvideo in the original
planar format. Comparison interleaves U/V losslessly; it never downsamples chroma.

The executed *.lab files are definitive. `prepare.py` preserves the research
transformation and expects the original workspace sibling m027-lt-lab/replay.lab.
`run.py` expects the original m029-formats directory, references and board tools;
it is not a deployment/bootstrap wrapper. Acquire the shared board lock, keep
console capture active, verify uploaded input/script hashes and run the existing
ve_lab in a separate guest directory per case. `input.bin` stays at offset zero.

Raw 1MiB output/guard dumps remain in the corresponding host scratch run-N
folders. Their SHA256 is archived; result.json records every comparison. Only
stream inputs, executed scripts, exact observed controls, logs and result data
are versioned here to avoid ten redundant 1MiB diagnostic buffers.
The negative probes do NOT establish higher-format hardware incapability.

`reject_probe.c` is a research-only copy of the BSD test at fbsd-media3cd9cd7;
`reject-probe.patch` shows every modification. `reject-run.py` compiles a separate
guest binary, verifies upload MD5, runs nine forced-control refusals and then
positive/recovery checks. Its JSON checkpoint skips already completed modes on
resume, as happened after correcting the initial errors-counter assertion.
The original cedrus_h264 binary and production driver are not modified.
