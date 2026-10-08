#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compare canon.c lists with independently observed JM ORACLE lines.

Only list decisions are compared, not other controls or decoded pixels.
Reference frame_num is retained in raw differences but excluded from identity:
JM split-field entries can report zero. Use single-POC-epoch inputs; this is not
an identity mapper for arbitrary streams with reused POCs.
"""
import argparse
import json
from pathlib import Path
import re


def oracle_records(path):
    records = []
    for line in path.read_text().splitlines():
        match = re.fullmatch(
            r'ORACLE fn (\d+) structure (\d+) poc (-?\d+) l0:(.*?) l1:(.*)', line)
        if match is None:
            continue
        frame_num, structure, poc = map(int, match.groups()[:3])
        lists = []
        for part in match.groups()[3:]:
            entries = [list(map(int, entry.split('/'))) for entry in part.split()]
            for entry in entries:
                if len(entry) != 5:
                    raise ValueError('invalid JM reference entry')
                # LongTermFrameIdx is immaterial for a short-term entry.
                if not entry[3]:
                    entry[4] = 0
            lists.append(entries)
        records.append(dict(fn=frame_num, structure=structure, poc=poc, lists=lists))
    return records


def canon_records(path):
    records, dpb = [], {}
    current = None
    for line in path.read_text().splitlines():
        if line.startswith('pic '):
            dpb = {}
            match = re.search(
                r'dp: nal \d+ fn (\d+) poc (-?\d+)/(-?\d+).* fl ([0-9a-f]+)', line)
            if match is None:
                raise ValueError('invalid canon picture')
            frame_num, top, bottom = map(int, match.groups()[:3])
            flags = int(match[4], 16)
            structure = 2 if flags & 4 else 1 if flags & 2 else 0
            poc = bottom if structure == 2 else top if structure == 1 else min(top, bottom)
            current = dict(fn=frame_num, structure=structure, poc=poc)
        elif line.startswith('  dpb '):
            match = re.fullmatch(
                r'  dpb (\d+) ts \d+ fn (\d+) pn (-?\d+) poc (-?\d+)/(-?\d+) '
                r'fields \d+ fl ([0-9a-f]+)', line)
            if match is None or current is None:
                raise ValueError('invalid canon DPB entry')
            slot, frame_num, number, top, bottom = map(int, match.groups()[:5])
            dpb[slot] = frame_num, number, top, bottom, int(match[6], 16)
        elif line.startswith('  sl '):
            match = re.search(r'type (\d+).* nref (\d+)/(\d+).* l0:(.*?) l1:(.*)', line)
            if match is None or current is None:
                raise ValueError('invalid canon slice')
            kind = int(match[1])
            if kind not in (0, 1, 2):
                raise ValueError('only P, B and I slices are supported')
            lists = []
            for index in range(2):
                count = int(match[index + 2]) + 1 if kind != 2 and (index == 0 or kind == 1) else 0
                entries = match[index + 4].split()[:count]
                if count > 32 or len(entries) != count:
                    raise ValueError('invalid canon list length')
                refs = []
                for entry in entries:
                    slot, fields = map(int, entry.split('.'))
                    if fields not in (1, 2, 3):
                        raise ValueError('invalid reference field selector')
                    frame_num, number, top, bottom, flags = dpb[slot]
                    structure = 0 if fields == 3 else fields
                    poc = bottom if fields == 2 else top if fields == 1 else min(top, bottom)
                    long_term = bool(flags & 4)
                    refs.append([frame_num, poc, structure, int(long_term),
                                 number if long_term else 0])
                lists.append(refs)
            records.append(dict(current, lists=lists))
    return records


def identity(picture):
    return dict(picture, lists=[[ref[1:] for ref in row] for row in picture['lists']])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--canon', type=Path, required=True)
    parser.add_argument('--jm', type=Path, required=True,
                        help='directory containing jm-lists-NAME/decode.txt')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('names', nargs='+')
    args = parser.parse_args()
    reports = []
    for name in args.names:
        own = canon_records(args.canon / (name + '.txt'))
        oracle = oracle_records(args.jm / ('jm-lists-' + name) / 'decode.txt')
        if not own or len(own) != len(oracle):
            raise ValueError(f'{name}: slice count mismatch ({len(own)}, {len(oracle)})')
        differences = [dict(picture=index, front_end=a, reference=b)
                       for index, (a, b) in enumerate(zip(own, oracle)) if a != b]
        identity_differences = [row for row in differences
                               if identity(row['front_end']) != identity(row['reference'])]
        reports.append(dict(
            name=name, slices=len(own), differences=differences,
            reference_identity_differences=identity_differences,
            identity_key='POC, field/frame structure, long-term flag/index; '
                         'reference frame_num not used (JM split fields may report 0)'))
        print(name, len(own), 'slices', len(differences), 'raw differences',
              len(identity_differences), 'identity differences')
    args.output.write_text(json.dumps(reports, indent=2) + '\n')
    return 1 if any(row['reference_identity_differences'] for row in reports) else 0


if __name__ == '__main__':
    raise SystemExit(main())
