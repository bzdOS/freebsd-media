#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""cmp_drv_linux.py DRIVER_TRACE LINUX_TRACE [linux_run_offset] — compare the register
state a slice is started with, as the driver programs it against Linux's own trace.

Order does not matter to the engine: what matters is the last value of each register
before the start command (0x224 = 8) and the SRAM tables.  Buffer addresses are
normalised (they differ by construction); everything else is compared literally.
"""
import re
import sys

ADDR_REGS = (0x230, 0x23c, 0x250, 0x254)


def compact(ops):
    """[(off, val)] with SRAM writes folded into ('SRAM', base, words)."""
    out, i = [], 0
    while i < len(ops):
        off, val = ops[i]
        if off == 0x2e0:
            base, i, words = val, i + 1, []
            while i < len(ops) and ops[i][0] == 0x2e4:
                words.append(ops[i][1])
                i += 1
            out.append(('SRAM', base, words))
        else:
            out.append(('W', off, val))
            i += 1
    return out


def state(ops):
    """register state before the start command, plus the SRAM tables."""
    regs, sram, started = {}, {}, False
    for op in ops:
        if op[0] == 'SRAM':
            _, base, words = op
            words = list(words)
            if base == 0x400:
                for i in range(len(words)):
                    if i % 8 in (3, 4, 5, 6):
                        words[i] = -1          # an address inside the picture table
            sram[base] = words
            continue
        _, off, val = op
        if off == 0x224 and val == 8:
            started = True
            break
        if off in ADDR_REGS:
            val = -1
        if off == 0x228:
            continue                    # acknowledge of a residual status, not programming
        regs[off] = val
    return regs, sram, started


def drv_slices(fn):
    """the driver's h264_trace: every write, split at the 0x0ec write."""
    tr = []
    for line in open(fn):
        m = re.match(r'W ([0-9a-f]{3}) ([0-9a-f]{8})', line.strip())
        if m:
            tr.append((int(m.group(1), 16), int(m.group(2), 16)))
    idx = [i for i, (o, _) in enumerate(tr) if o == 0x0ec] + [len(tr)]
    return [tr[idx[k]:idx[k + 1]] for k in range(len(idx) - 1)]


def linux_slices(fn):
    """the vetrap trace: <n> cpu0 W32 VE <off> <val>, runs split at VE 0x0ec."""
    ents = []
    for line in open(fn):
        x = line.split()
        if len(x) >= 6:
            ents.append((int(x[0]), x[2], x[3], int(x[4], 16), int(x[5], 16)))
    idx = [i for i, e in enumerate(ents) if e[2] == 'VE' and e[1] == 'W32' and e[3] == 0x0ec]
    idx.append(len(ents))
    return [[(e[3], e[4]) for e in ents[idx[k]:idx[k + 1]] if e[1] == 'W32' and e[2] == 'VE']
            for k in range(len(idx) - 1)]


def main():
    drv, lin = sys.argv[1], sys.argv[2]
    off = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    D = [state(compact(s)) for s in drv_slices(drv)]
    L = [state(compact(s)) for s in linux_slices(lin)]
    print('драйвер: %d слайсов; Linux: %d прогонов' % (len(D), len(L)))
    for k in range(min(3, len(D), len(L) - off)):
        rd, sd, st_d = D[k]
        rl, sl, st_l = L[off + k]
        print('\n=== слайс %d (START у драйвера: %s, у Linux: %s) ===' % (k, st_d, st_l))
        for o in sorted(set(rd) | set(rl)):
            a, b = rd.get(o), rl.get(o)
            if a != b:
                print('  %-6s драйвер=%-12s Linux=%-12s' % ('%#05x' % o,
                      'нет' if a is None else '%#x' % a, 'нет' if b is None else '%#x' % b))
        for base in sorted(set(sd) | set(sl)):
            wa, wb = sd.get(base), sl.get(base)
            if wa == wb:
                continue
            if wa is None or wb is None:
                print('  SRAM %#05x: у драйвера %s, у Linux %s' % (base,
                      'нет' if wa is None else '%d слов' % len(wa),
                      'нет' if wb is None else '%d слов' % len(wb)))
                continue
            n = sum(1 for x, y in zip(wa, wb) if x != y)
            print('  SRAM %#05x: %d слов, отличий %d' % (base, len(wa), n))
            for i, (x, y) in enumerate(zip(wa, wb)):
                if x != y:
                    print('     слово %d: драйвер=%#x Linux=%#x' % (i, x, y))
                    if i > 12:
                        break


if __name__ == '__main__':
    main()
