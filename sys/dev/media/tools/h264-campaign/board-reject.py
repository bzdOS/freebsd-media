#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Unsupported-format assay: the driver must refuse the controls, count nothing, keep decoding.

Pushes cedrus_h264_reject.c (built on the guest) and the first packets of each named
corpus stream (md5 checked), runs the rejection test on each, the injection cases on a
supported stream, the positive control, and an ordinary decode afterwards; reads the
driver's counters before and after. Needs the rig of board-bootstrap.sh.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path('/opt/bzdos')
BASE = ROOT / 'linux-work/h264-traces'
HERE = Path(__file__).resolve().parent
TESTS = HERE.parent.parent / 'tests'
STREAMS = ['FREXT1_TANDBERG_A', 'Hi422FR1_SONY_A', 'Hi422FR13_SONY_A', 'Hi422FREXT16_SONY_A',
           'FREH10-1', 'PPH10I1_Panasonic_A', 'PPH444I4_Mitsubishi_A', 'PPCV444I4_Mitsubishi_A',
           'SP1_BT_A']
INJECT = ['fmo', 'sp', 'si', 'plane', 'plane_id', '422', '444', '10bit', '14bit']
COUNTERS = 'pictures errors rejects timeouts'.split()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--supported', default='f3', help='stream from initramfs-h264/streams')
    p.add_argument('--head-kb', type=int, default=3072)
    a = p.parse_args()
    a.output.mkdir(exist_ok=False)
    catalog = json.loads((ROOT / 'linux-work/jvt/catalog.json').read_text())
    opts = ['-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8']
    ssh = ['ssh'] + opts + ['root@192.0.2.82']

    def sh(cmd, timeout=60):
        r = subprocess.run(ssh + [cmd], capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode(errors='replace')

    def counters():
        _, out = sh('for c in ' + ' '.join(COUNTERS) + '; do sysctl -n dev.cedrus.0.$c; done')
        return dict(zip(COUNTERS, (int(x) for x in out.split())))

    def push(local, remote):
        subprocess.run(['scp'] + opts + [str(local), 'root@192.0.2.82:' + remote],
                       check=True, capture_output=True, timeout=90)
        want = hashlib.md5(Path(local).read_bytes()).hexdigest()
        _, got = sh('md5 -q ' + remote)
        if got.strip() != want:
            raise RuntimeError(f'upload checksum mismatch {local}')
        return want

    res = {'cases': []}
    with (BASE / '.board-cycle.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        res['md5'] = {'cedrus_h264_reject.c': push(TESTS / 'cedrus_h264_reject.c',
                                                   '/tmp/ve/cedrus_h264_reject.c'),
                      'h264ctl.h': push(TESTS / 'h264ctl.h', '/tmp/ve/h264ctl.h')}
        rc, out = sh('cd /tmp/ve && cc -I/tmp/shim -I/tmp/uapi -o cedrus_h264_reject '
                     'cedrus_h264_reject.c && uname -v && kldstat && md5 /tmp/cedrus.ko /tmp/media.ko',
                     timeout=180)
        (a.output / 'guest-before.txt').write_text(out)
        if rc:
            raise SystemExit('guest build failed:\n' + out)
        res['before'] = counters()
        runs = []
        for name in STREAMS:
            clip = a.output / (name + '.264')
            # The head of the Annex B file: ffmpeg cannot remux every one of these
            # (separate colour planes), and the test only needs the first pictures.
            clip.write_bytes(Path(catalog[name]['bitstream']).read_bytes()[:a.head_kb * 1024])
            remote = '/tmp/ve/rej-' + name + '.264'
            runs.append((name, None, push(clip, remote), remote, False))
        sup = ROOT / 'linux-work/initramfs-h264/streams' / (a.supported + '.264')
        sup_remote = '/tmp/ve/rej-' + a.supported + '.264'
        sup_md5 = push(sup, sup_remote)
        for what in INJECT:
            runs.append((a.supported, what, sup_md5, sup_remote, False))
        runs.append((a.supported, None, sup_md5, sup_remote, True))
        for name, what, md5, remote, accept in runs:
            cmd = './cedrus_h264_reject ' + ('-a ' if accept else '') + \
                  (f'-i {what} ' if what else '') + remote
            c0 = counters()
            rc, out = sh('cd /tmp/ve && ' + cmd, timeout=60)
            c1 = counters()
            ok = rc == 0 and c0 == c1 and \
                ('ACCEPTED as expected' if accept else 'REJECTED as expected') in out
            row = dict(stream=name, inject=what, accept=accept, md5=md5, exit=rc,
                       counters_unchanged=c0 == c1, ok=ok, output=out.splitlines())
            res['cases'].append(row)
            print(('OK  ' if ok else 'BAD ') + name + (' +' + what if what else '') +
                  (' (positive control)' if accept else ''), flush=True)
            sh('rm -f ' + remote) if remote != sup_remote else None
        # The engine still decodes after all that.
        rc, out = sh('cd /tmp/ve && ./cedrus_h264 f3.264 f3.yuv', timeout=120)
        (a.output / 'f3-after.txt').write_text(out)
        res['f3_after'] = dict(exit=rc, passed=len(re.findall(r'^picture .*: PASS ', out, re.M)),
                               fail=bool(re.search(r'FAIL|ioctl|did not complete', out)))
        res['after'] = counters()
        res['ok'] = all(c['ok'] for c in res['cases']) and rc == 0 and not res['f3_after']['fail']
    (a.output / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps({k: res[k] for k in ('before', 'after', 'f3_after', 'ok')}))
    return 0 if res['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
