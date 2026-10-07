#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Summarize measured VE register writes and SRAM tables, one slice per 0xec."""
import argparse
import json
from pathlib import Path


def summarize(lines):
    runs = []
    current = None
    pointer = None
    for line in lines:
        row = line.split()
        if len(row) < 6:
            raise ValueError(f'malformed trace line: {line!r}')
        if row[2:4] != ['W32', 'VE']:
            continue
        offset, value = int(row[4], 16), int(row[5], 16)
        if offset == 0xec:
            current = {'first_entry': int(row[0]), 'registers': {}, 'sram': {}}
            runs.append(current)
            pointer = None
        if current is None:
            continue
        repeat = int(row[6][1:]) if len(row) > 6 else 1
        if offset == 0x2e0:
            pointer = value
        elif offset == 0x2e4:
            if pointer is None:
                raise ValueError('SRAM data without pointer')
            for _ in range(repeat):
                current['sram'][pointer] = value
                pointer += 4
        else:
            current['registers'][f'{offset:03x}'] = f'{value:08x}'
    for current in runs:
        sram = current.pop('sram')
        current['table'] = []
        for base in range(0x400, 0x620, 32):
            words = [sram.get(base + 4 * i, 0) for i in range(8)]
            if any(words):
                current['table'].append({'slot': (base - 0x400) // 32,
                                         'words': [f'{v:08x}' for v in words],
                                         'mv_half_distance': words[6] - words[5]})
        current['lists'] = {f'{base:03x}': [f'{sram[p]:08x}' for p in range(base, base + 32, 4) if p in sram]
                            for base in (0x640, 0x664)}
    return runs


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = summarize(args.trace.read_text().splitlines())
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(result)} slice runs')
