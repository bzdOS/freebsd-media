#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check uapi/linux/media.h against a mechanically extracted fact sheet.

  media_facts_check.py FACTS.txt [--cc clang] [--sysroot DIR] [--keep]

Parses the "struct", "member", "const" and "ioctl" lines of FACTS.txt, writes a
C file of _Static_asserts (sizeof/offsetof of members, constant values, ioctl
direction/type/number/size decoded with the Linux ioctl bit layout) and compiles
it against ../../uapi and the shim directory. Exit status 0 means every fact holds.
"""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
UAPI = os.path.join(HERE, '..', '..', 'uapi')
SHIM = os.path.join(HERE, '..', '..', 'shim')


def parse(path):
    structs, consts, ioctls = [], [], []
    cur = None
    for line in open(path):
        line = line.rstrip('\n')
        m = re.match(r'struct (\w+) size (\d+)', line)
        if m:
            cur = {'name': m.group(1), 'size': int(m.group(2)), 'members': []}
            structs.append(cur)
            continue
        m = re.match(r'\s+member (\w+) : .* @(\d+)$', line)
        if m and cur is not None:
            # members of unnamed inner structs are not addressable by offsetof
            if line.startswith('      member') and 'struct (unnamed' in line:
                continue
            cur['members'].append((m.group(1), int(m.group(2))))
            continue
        m = re.match(r'const (\w+) (0x[0-9a-f]+|\d+)$', line)
        if m:
            consts.append((m.group(1), int(m.group(2), 0)))
            continue
        m = re.match(r'ioctl (\w+) dir=(\d+) type=(\d+) nr=(\d+) size=(\d+)', line)
        if m:
            ioctls.append((m.group(1),) + tuple(int(x) for x in m.groups()[1:]))
    return structs, consts, ioctls


def generate(structs, consts, ioctls):
    o = ['#include <stddef.h>', '#include <linux/media.h>', '',
         '/* Linux-layout decode of an ioctl number: dir 2 bits | size 14 | type 8 | nr 8,',
         '   dir bit0 = write, bit1 = read; the header is FreeBSD-style, so the',
         '   decode is done from the IOC_* parts of <sys/ioccom.h>. */']
    o.append('#define F_DIR(c) ((((c) & IOC_IN) ? 1 : 0) | (((c) & IOC_OUT) ? 2 : 0))')
    o.append('#define F_SIZE(c) IOCPARM_LEN(c)')
    o.append('#define F_TYPE(c) IOCGROUP(c)')
    o.append('#define F_NR(c) ((c) & 0xff)')
    for s in structs:
        n = s['name']
        o.append('_Static_assert(sizeof(struct %s) == %d, "sizeof %s");' % (n, s['size'], n))
        for mem, off in s['members']:
            o.append('_Static_assert(offsetof(struct %s, %s) == %d, "%s.%s");' % (n, mem, off, n, mem))
    for n, v in consts:
        o.append('_Static_assert((unsigned long long)(%s) == 0x%xULL, "%s");' % (n, v, n))
    for n, d, t, nr, sz in ioctls:
        o.append('_Static_assert(F_DIR(%s) == %d, "%s dir");' % (n, d, n))
        o.append('_Static_assert(F_TYPE(%s) == %d, "%s type");' % (n, t, n))
        o.append('_Static_assert(F_NR(%s) == %d, "%s nr");' % (n, nr, n))
        o.append('_Static_assert(F_SIZE(%s) == %d, "%s size");' % (n, sz, n))
    o.append('int main(void) { return 0; }')
    return '\n'.join(o) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('facts')
    ap.add_argument('--cc', default='clang')
    ap.add_argument('--sysroot', default='/opt/bzdos/sysroots/aarch64-15.1')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    structs, consts, ioctls = parse(a.facts)
    tmp = tempfile.mkdtemp(prefix='media-facts.')
    src = os.path.join(tmp, 'check.c')
    with open(src, 'w') as f:
        f.write('#include <sys/ioccom.h>\n' + generate(structs, consts, ioctls))
    cmd = [a.cc, '--target=aarch64-unknown-freebsd15.1', '--sysroot=' + a.sysroot, '-std=gnu11', '-Wall', '-Werror', '-fsyntax-only', '-I' + SHIM, '-I' + UAPI, src]
    r = subprocess.run(cmd)
    print('%d structs, %d members, %d constants, %d ioctls: %s' % (
        len(structs), sum(len(s['members']) for s in structs), len(consts), len(ioctls),
        'OK' if r.returncode == 0 else 'FAIL'))
    if not a.keep and r.returncode == 0:
        os.remove(src); os.rmdir(tmp)
    else:
        print('kept', src)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
