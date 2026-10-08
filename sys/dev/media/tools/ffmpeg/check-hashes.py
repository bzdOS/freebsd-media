#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
#
# check-hashes.py clip player-hashes.txt — compare what fbplay -H recorded for every presented frame (hash of the NV12
# picture read back from the decoder's dma-buf, by pts) with the host's software decode of the same clip
# (ffmpeg -pix_fmt nv12 -f framemd5). Prints how many presented frames match bit for bit, how many differ, and how
# many of the clip's frames were not presented (dropped, or not reached).
import subprocess, sys

clip, got = sys.argv[1], sys.argv[2]
out = subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-an", "-pix_fmt", "nv12", "-f", "framemd5", "-"],
                     capture_output=True, text=True, check=True).stdout
tb = 1.0
ref = {}
for l in out.splitlines():
    if l.startswith("#tb"):
        n, d = l.split(":")[1].strip().split("/")
        tb = int(n) / int(d)
    elif l and not l.startswith("#"):
        f = [x.strip() for x in l.split(",")]
        ref[round(int(f[2]) * tb, 4)] = f[5]
ok = bad = unknown = 0
badl = []
seen = set()
for l in open(got):
    p, h = l.split()
    k = round(float(p), 4)
    seen.add(k)
    if k not in ref:
        unknown += 1
    elif ref[k] == h:
        ok += 1
    else:
        bad += 1
        badl.append(k)
print(f"{clip}: reference {len(ref)} frames; presented {ok + bad + unknown}: identical {ok}, different {bad}, "
      f"no such pts {unknown}; reference frames not presented {len(set(ref) - seen)}")
if badl:
    print("first different pts:", badl[:10])
