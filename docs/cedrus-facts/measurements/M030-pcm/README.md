# M030 PCM probes

See ../M030-hardware-ceiling.md. `pcm-fixtures.py` constructs standards-conforming
single-picture 352x288 CAVLC H.264 streams using only I_PCM macroblocks. Every
sample is known in advance. It verifies complete output with ffmpeg; monochrome
comparison uses yuv420p to avoid ffmpeg's limited-range-to-gray conversion.
It is a bitstream generator, not a decoder or software fallback.

pcm0/1/2/3 correspond to monochrome/420/422/444 input syntax. The executed
chroma-N.lab scripts vary only the PICSIZE chroma selector, with added passive
status/error/position observations. Values4..7 are diagnostic candidates inside
the described three-bit field, not production encodings. All buffers/addresses
are unchanged. The same four output/MV guards as M029 are checked on every run.

The research helpers retain the original m030-scope layout under
linux-work/h264-traces; prepare.py also expects sibling m027-lt-lab/replay.lab.
The definitive inputs/scripts/results are archived here. Regenerate references
with pcm-fixtures.py. Raw1MiB output dumps stay in host scratch; SHA256 is archived.
The two successful pcm1 replays use the same420 input with selectors1 and0;
selector0 is a deliberately mismatched-control observation, not a valid mono input.

Ordinary native-driver PCM tests verify both uploaded stream/reference MD5s.
The mono ERROR is an expected reproduced failure; the420 positive control and
subsequent f3/weighted-gray recovery pass. No production driver was altered.
