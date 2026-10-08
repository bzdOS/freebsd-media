#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# cmp-nv12.py clip dump.nv12 width height — compare fbplay -D output (every presented frame, raw NV12, in order,
# start of the clip) with the host's software decode of the same clip: per-plane maximum absolute difference, the
# share of differing samples and a histogram of the differences. For MPEG-2 the engine is expected to differ from
# FFmpeg's IDCT by at most +-2 (M038).
import subprocess, sys
import numpy as np

clip, dump, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
fs = w * h * 3 // 2
a = np.fromfile(dump, dtype=np.uint8)
n = len(a) // fs
ref = subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-an", "-frames:v", str(n), "-pix_fmt", "nv12", "-f", "rawvideo", "-"],
                     capture_output=True, check=True).stdout
b = np.frombuffer(ref, dtype=np.uint8)
n = min(n, len(b) // fs)
a = a[:n * fs].reshape(n, fs).astype(np.int16)
b = b[:n * fs].reshape(n, fs).astype(np.int16)
d = a - b
y, c = d[:, :w * h], d[:, w * h:]
for name, p in (("Y", y), ("UV", c)):
    ad = np.abs(p)
    print(f"{name}: frames {n}, max |diff| {ad.max()}, differing samples {100.0 * (ad > 0).mean():.2f}%, "
          f"hist 0:{(ad == 0).sum()} 1:{(ad == 1).sum()} 2:{(ad == 2).sum()} >2:{(ad > 2).sum()}")
print("frames with any difference:", int((np.abs(d).max(axis=1) > 0).sum()), "of", n)
