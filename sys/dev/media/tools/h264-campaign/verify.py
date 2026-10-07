#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Check short single-POC-epoch rig clips against uncropped ffmpeg luma."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def compare(console, paths):
    flat = re.sub(r'\[\s*\d+\.\d+\]\s*', '', console).replace('\r', '')
    if 'TRACE-DONE' not in flat or re.search(r'failed|FAIL|no device', flat):
        raise ValueError('missing completion or rig reported failure')
    sections = list(re.finditer(r'STREAM\s+(0x[0-9a-fA-F]+)', flat))
    if [int(s[1], 16) for s in sections] != list(range(len(paths))):
        raise ValueError('stream count/order mismatch')
    report = []
    for index, path in enumerate(paths):
        section = flat[sections[index].end():sections[index + 1].start()
                       if index + 1 < len(sections) else len(flat)]
        variant = re.search(r'variant\s+(0x[0-9a-fA-F]+)', section)
        if not variant or int(variant[1], 16):
            raise ValueError('reference comparison requires variant 0')
        values = re.findall(r'PIC\s*' + r'\s+'.join([r'(0x[0-9a-fA-F]+)'] * 5), section)
        pics = [[int(x, 16) for x in row] for row in values]
        if not pics or [p[0] for p in pics] != list(range(len(pics))):
            raise ValueError(f'stream {index}: missing or unordered PIC records')
        if any(p[4] & 4 or (i > 0 and p[4] & 8) for i, p in enumerate(pics)):
            raise ValueError('multiple POC epochs/MMCO5 need explicit display-order mapping')
        for pic in pics:
            if pic[1] >= 1 << 63:
                pic[1] -= 1 << 64
        probe = json.loads(subprocess.check_output([
            'ffprobe', '-v', 'error', '-apply_cropping', '0', '-select_streams', 'v:0',
            '-show_entries', 'stream=width,height,pix_fmt', '-of', 'json', str(path)]))['streams'][0]
        if probe['pix_fmt'] != 'yuv420p':
            raise ValueError('only 8-bit 4:2:0 luma comparison is supported')
        raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-xerror', '-apply_cropping', '0',
                                       '-i', str(path), '-vf', 'extractplanes=y',
                                       '-f', 'rawvideo', 'pipe:1'])
        size = probe['width'] * probe['height']
        if len(raw) % size or len(raw) // size != len(pics):
            raise ValueError(f'stream {index}: reference/frame count mismatch')
        hashes = []
        for offset in range(0, len(raw), size):
            value = 2166136261
            for byte in raw[offset:offset + size]:
                value = ((value ^ byte) * 16777619) & 0xffffffff
            hashes.append(value)
        actual = [p[3] for p in sorted(pics, key=lambda p: p[1])]
        report.append({'stream': index, 'file': str(path),
                       'input_md5': hashlib.md5(path.read_bytes()).hexdigest(),
                       'frames': len(pics), 'matching_luma_hashes': sum(a == b for a, b in zip(actual, hashes)),
                       'expected': [f'{h:08x}' for h in hashes],
                       'actual': [f'{h:08x}' for h in actual]})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('console', type=Path)
    parser.add_argument('streams', type=Path, nargs='+', help='exact embedded clips, in rig order')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = compare(args.console.read_text(), args.streams)
        passed = all(r['frames'] == r['matching_luma_hashes'] for r in report)
        result = {'luma_pass': passed, 'scope': '8-bit luma FNV1a only; no chroma comparison',
                  'streams': report}
    except (ValueError, subprocess.SubprocessError) as exc:
        result = {'luma_pass': False, 'error': str(exc)}
        passed = False
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
