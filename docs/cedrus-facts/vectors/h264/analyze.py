#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""analyze.py — diff the variant runs of a vetrap H.264 trace against their control runs.

Trace line format (vetrace.py):  <idx> cpu<N> W32|R32 <PAGE> <off> <val>
A "run" is one slice programming, delimited by a write to VE 0x0ec (as in tr.py).

Usage: analyze.py <trace> <entry-variant-list>   e.g. analyze.py x.trace.txt 0,1,0,2
"""
import sys
from collections import defaultdict


def load(fn):
    ents = []
    for l in open(fn):
        x = l.split()
        if len(x) < 6:
            continue
        ents.append((int(x[0]), x[2], x[3], int(x[4], 16), int(x[5], 16)))
    return ents


def runs(ents):
    idx = [i for i, e in enumerate(ents) if e[2] == 'VE' and e[1] == 'W32' and e[3] == 0xec]
    idx.append(len(ents))
    return [ents[idx[k]:idx[k + 1]] for k in range(len(idx) - 1)]


def compact(run):
    """('W'|'R', off, val) and ('SRAM', base, [words]) — same shape as tr.py."""
    out, i = [], 0
    while i < len(run):
        e = run[i]
        if e[2] == 'VE' and e[3] == 0x2e0 and e[1] == 'W32':
            base, i = e[4], i + 1
            words = []
            while i < len(run) and run[i][3] == 0x2e4 and run[i][1] == 'W32':
                words.append(run[i][4])
                i += 1
            out.append(('SRAM', base, words))
        else:
            out.append((e[1][0], e[3], e[4], e[2]))
            i += 1
    return out


def flat(ops):
    """register writes/reads as {(kind,off):val}; SRAM as {(kind,base,word_i):val}."""
    d = {}
    for op in ops:
        if op[0] == 'SRAM':
            _, base, words = op
            for j, w in enumerate(words):
                d[('S', base, j)] = w
        else:
            kind, off, val, _pg = op
            d[(kind, off)] = val
    return d


def diff(a, b, limit=40):
    ka, kb = flat(a), flat(b)
    keys = sorted(set(ka) | set(kb), key=lambda k: (k[0], k[1], k[2] if len(k) > 2 else 0))
    rows = []
    for k in keys:
        va, vb = ka.get(k), kb.get(k)
        if va != vb:
            rows.append((k, va, vb))
    return rows[:limit], len(rows)


def fmt(k):
    if k[0] == 'S':
        return 'SRAM %#05x[%d]' % (k[1], k[2])
    return '%s %#05x' % ('wr' if k[0] == 'W' else 'rd', k[1])


def main():
    trace, variants = sys.argv[1], [int(x) for x in sys.argv[2].split(',')]
    ents = load(trace)
    R = runs(ents)
    print('trace %s: %d entries, %d runs, variants %s' % (trace, len(ents), len(R), variants))
    if not R:
        return
    # which run belongs to which entry: the rig prints one STREAM marker per entry, and
    # each entry contributes 1..H264_MAX_PICS pictures = runs.  Split runs evenly only as a
    # hint; the printed run index is what the diff uses.
    n = len(variants)
    per = len(R) // n
    print('runs per entry (assuming even split): %d' % per)
    ctrl = [i for i, v in enumerate(variants) if v == 0]
    for i, v in enumerate(variants):
        if v == 0:
            continue
        # control for this variant: the nearest earlier control entry, else the first
        c = max([j for j in ctrl if j < i], default=ctrl[0] if ctrl else None)
        if c is None:
            continue
        for k in range(per):
            ra, rb = compact(R[c * per + k]), compact(R[i * per + k])
            rows, total = diff(ra, rb)
            print('\n--- run %d: entry %d (control) vs entry %d (variant %d): %d differing locations' %
                  (k, c, i, v, total))
            for key, va, vb in rows:
                print('    %-16s control=%s  variant=%s' % (fmt(key),
                      'None' if va is None else '%#x' % va,
                      'None' if vb is None else '%#x' % vb))


if __name__ == '__main__':
    main()
