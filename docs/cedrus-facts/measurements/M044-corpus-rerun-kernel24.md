# M044 - full H.264 corpus rerun, guest kernel #24

2026-10-10. Same procedure as M028: `board-bootstrap.sh` (modules rebuilt from the
current tree and md5-verified on the guest: cedrus.ko b48110a5267704e4b915fb702ab1b12b,
media.ko ecb94376cd8174457454382d2837262a, dmabuf_base.ko 877facc5b960469094af8d66e72fb22f),
then `h264-campaign/board-corpus.py --frames 0 --names <the 176 streams of M028 full-fixed.json>`.
Guest kernel: FreeBSD 15.1-RC3 #24. Every upload md5-checked, every reference frame needs a PASS line.

Result: 176/176 streams exact, 19,028/19,028 full Y/C frames, 0 reference warnings,
0 FAIL/ioctl/incomplete lines, 165 references equal to the official corpus YUV.
Compared per stream with M028 `full-fixed.json`: frame counts, input/reference md5s,
pass counts and verdicts are identical for all 176 streams (no difference).
Wall time about 18 minutes (03:06-03:25 MSK), one stream at a time (no batching needed).

Not run: the 55 streams the strict parser refuses (ten-bit, 4:2:2/4:4:4, FMO, SP/SI).
Driver error/timeout sysctl counters were not read after the run: the guest was
rebooted by someone else right after the run finished.
Evidence: linux-work/h264-traces/m044-campaign-k24/ (result.json and per-stream logs).
