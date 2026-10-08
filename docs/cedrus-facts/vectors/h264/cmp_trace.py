#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""cmp_trace.py FAIL.txt PASS.txt — diff two H.264 register traces slice by slice.

The traces come from dev.cedrus.0.h264_trace (every bus_write_4 of one decode run).
Addresses depend on where the allocator happened to place the buffers, so they are
normalised to 'A': the address registers 0x230/0x23c/0x250/0x254, and words 3..6 of
each 8-word picture-table entry in the SRAM 0x400 stream.
"""
import re
import sys


def parse(fn):
    out = []
    sram_base = None
    sram_i = 0
    for line in open(fn):
        m = re.match(r'W ([0-9a-f]{3}) ([0-9a-f]{8})', line.strip())
        if not m:
            continue
        off, val = int(m.group(1), 16), int(m.group(2), 16)
        if off == 0x2e0:
            sram_base, sram_i = val, 0
        elif off == 0x2e4:
            if sram_base == 0x400 and (sram_i % 8) in (3, 4, 5, 6):
                val = -1                      # an address inside the picture table
            sram_i += 1
        elif off in (0x230, 0x23c, 0x250, 0x254):
            val = -1
        out.append((off, val))
    return out


def slices(tr):
    idx = [i for i, (o, _) in enumerate(tr) if o == 0x0ec]
    idx.append(len(tr))
    return [tr[idx[k]:idx[k + 1]] for k in range(len(idx) - 1)]


def main():
    a = slices(parse(sys.argv[1]))          # FAIL
    b = slices(parse(sys.argv[2]))          # PASS
    print('%s: %d slices; %s: %d slices' % (sys.argv[1], len(a), sys.argv[2], len(b)))
    for k in range(min(len(a), len(b))):
        sa, sb = a[k], b[k]
        if len(sa) != len(sb):
            print('slice %d: length %d vs %d' % (k, len(sa), len(sb)))
            continue
        diffs = [(i, o, va, vb) for i, ((o, va), (_, vb)) in enumerate(zip(sa, sb)) if va != vb]
        print('slice %d: %d writes, %d differing' % (k, len(sa), len(diffs)))
        for i, o, va, vb in diffs[:12]:
            print('    #%d off %#05x  FAIL=%s  PASS=%s' %
                  (i, o, 'addr' if va == -1 else '%#x' % va,
                   'addr' if vb == -1 else '%#x' % vb))
        if len(diffs) > 12:
            print('    ... and %d more' % (len(diffs) - 12))


if __name__ == '__main__':
    main()
