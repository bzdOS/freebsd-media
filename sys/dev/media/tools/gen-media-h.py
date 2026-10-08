#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Generate uapi/linux/media.h from uapi/media.spec.

  gen-media-h.py [SPEC [OUT]]

The spec is JSON (structs, constants with numeric values, ioctls). Output form:
bare hexadecimal constants grouped by family and sorted by value then name,
one struct member per line in a tab-aligned column, and ioctls written through
the MEDIA_IOCTL() helper with direction/number/argument as separate parameters.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = os.path.join(HERE, '..', 'uapi', 'media.spec')
OUT = os.path.join(HERE, '..', 'uapi', 'linux', 'media.h')


def tabs_to(col, text_len):
    """Tabs needed to move from text_len to column col (both in chars, tab=8)."""
    n = 0
    while text_len < col:
        text_len = (text_len // 8 + 1) * 8
        n += 1
    return max(n, 1)


def decl(m, tcol, indent):
    t = m['type']
    name = m['name']
    if t.endswith(' *'):
        t = t[:-2]
        name = '*' + name
    if 'count' in m:
        name += '[%d]' % m['count']
    return '%s%s%s%s;' % ('\t' * indent, t, '\t' * tabs_to(tcol, len(t) + 8 * indent) if tcol else '\t', name)


def emit_members(members, indent, out):
    plain = [m for m in members if 'type' in m]
    tcol = (((max(len(m['type'][:-2] if m['type'].endswith(' *') else m['type']) for m in plain) + 8 * indent) // 8) + 1) * 8 if plain else 0
    for m in members:
        if 'fields' in m:
            out.append('\t' * indent + 'struct {')
            emit_members(m['fields'], indent + 1, out)
            out.append('\t' * indent + '} %s;' % m['name'])
        else:
            out.append(decl(m, tcol, indent))


def main():
    spec = json.load(open(sys.argv[1] if len(sys.argv) > 1 else SPEC))
    dst = sys.argv[2] if len(sys.argv) > 2 else OUT
    o = []
    o += ['/* SPDX-License-Identifier: BSD-3-Clause */',
          '/*',
          ' * GENERATED FILE - do not edit.',
          ' * Source: sys/dev/media/uapi/media.spec',
          ' * Generator: sys/dev/media/tools/gen-media-h.py',
          ' *',
          ' * Media controller userspace interface: constants, structures and ioctls.',
          ' */',
          '#ifndef _UAPI_LINUX_MEDIA_H',
          '#define _UAPI_LINUX_MEDIA_H',
          '',
          '#include <linux/types.h>',
          '#include <linux/ioctl.h>',
          '']
    fams = {}
    for c in spec['constants']:
        fams.setdefault(c['family'], []).append(c)
    for f in spec['family_order']:
        cs = sorted(fams.get(f, []), key=lambda c: (c['value'], c['name']))
        o.append('/* %s */' % spec['family_doc'][f])
        w = max(len(c['name']) for c in cs) + 1
        for c in cs:
            o.append('#define %s%s0x%x' % (c['name'], '\t' * tabs_to(((len('#define ') + w) // 8 + 1) * 8, len('#define ') + len(c['name'])), c['value']))
        o.append('')
    for s in spec['structs']:
        o.append('/* %s */' % s['doc'])
        o.append('struct %s {' % s['name'])
        emit_members(s['members'], 1, o)
        if 'union' in s:
            o.append('\tunion {')
            emit_members(s['union'], 2, o)
            o.append('\t};')
        o.append('}%s;' % (' __attribute__((packed))' if s['packed'] else ''))
        o.append('')
    o += ['/*',
          ' * Helper pair for the ioctl numbers below: MEDIA_IOCTL(dir, nr, argtype)',
          ' * selects the _IO/_IOR/_IOW/_IOWR form by token pasting on dir.',
          ' */',
          '#define MEDIA_IOCTL_DIR_N(grp, nr, argt)\t_IO(grp, nr)',
          '#define MEDIA_IOCTL_DIR_R(grp, nr, argt)\t_IOR(grp, nr, argt)',
          '#define MEDIA_IOCTL_DIR_W(grp, nr, argt)\t_IOW(grp, nr, argt)',
          '#define MEDIA_IOCTL_DIR_RW(grp, nr, argt)\t_IOWR(grp, nr, argt)',
          '#define MEDIA_IOCTL(dir, nr, argt)\tMEDIA_IOCTL_DIR_##dir(0x%02x, nr, argt)' % spec['ioctl_type'],
          '']
    w = max(len(i['name']) for i in spec['ioctls'])
    for i in spec['ioctls']:
        o.append('#define %s%sMEDIA_IOCTL(%s, 0x%02x, %s)' % (
            i['name'], '\t' * tabs_to(((len('#define ') + w) // 8 + 1) * 8, len('#define ') + len(i['name'])),
            i['dir'], i['nr'], i.get('arg', 'void')))
    o += ['', '#endif /* _UAPI_LINUX_MEDIA_H */', '']
    open(dst, 'w').write('\n'.join(o))


if __name__ == '__main__':
    main()
