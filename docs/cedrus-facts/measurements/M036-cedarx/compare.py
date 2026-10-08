#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Normalize observed MB32 output storage for pixel comparison, not decoding."""
from pathlib import Path
import hashlib
import json
import re
import sys

case = Path(sys.argv[1])
reference = Path(sys.argv[2]).read_bytes()


def untile(raw, width, height):
    assert width % 32 == height % 32 == 0 and len(raw) == width * height
    result = bytearray(width * height)
    offset = 0
    for by in range(0, height, 32):
        for bx in range(0, width, 32):
            for y in range(32):
                start = (by + y) * width + bx
                result[start:start + 32] = raw[offset + y * 32:offset + y * 32 + 32]
            offset += 1024
    return result


reports = []
pattern = r'PICTURE (\d+).*format=(\d+) size=(\d+)x(\d+) stride=(\d+) bytes=(\d+)\+(\d+)'
for index, fmt, width, height, stride, ysize, csize in re.findall(pattern, (case / 'probe.txt').read_text()):
    index, fmt, w, h, stride, ysize, csize = map(int, (index, fmt, width, height, stride, ysize, csize))
    raw = (case / f'picture-{index}.bin').read_bytes()
    assert len(raw) == ysize + csize and fmt == 7
    y = untile(raw[:ysize], stride, ysize // stride)
    c = untile(raw[ysize:], stride, csize // stride)
    visible_y = b''.join(y[row * stride:row * stride + w] for row in range(h))
    visible_uv = b''.join(c[row * stride:row * stride + w] for row in range(h // 2))
    planar = visible_y + visible_uv[0::2] + visible_uv[1::2]
    size = w * h * 3 // 2
    ref = reference[index * size:(index + 1) * size]
    report = dict(frame=index, exact=len(ref) == size and planar == ref,
                  different_bytes=sum(a != b for a, b in zip(planar, ref)),
                  reference_bytes=len(ref), bytes=len(planar),
                  output_sha256=hashlib.sha256(planar).hexdigest())
    reports.append(report)
    print(json.dumps(report), flush=True)
log = (case / 'probe.txt').read_text()
guard = re.findall(r'^GUARD (\d+) offset=(0x[0-9a-f]+) intact=([01])$', log, re.M)
result = dict(frames=reports, guards=len(guard),
              guards_intact=bool(guard) and all(x[2] == '1' for x in guard),
              expected_frames=len(reference) // (w * h * 3 // 2) if reports else None,
              all_expected_exact=bool(reports) and all(x['exact'] for x in reports) and
                  len(reports) * reports[0]['bytes'] == len(reference))
(case / 'pixel-comparison.json').write_text(json.dumps(result, indent=2) + '\n')
