#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Capture a Linux rig trace and restore FreeBSD; never flash persistent media."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path('/opt/bzdos')
FILES = ('kernel', 'bananapi-min.dtb', 'microkernel-dbg.uimg')


def md5(path):
    with open(path, 'rb') as src:
        return hashlib.file_digest(src, 'md5').hexdigest()


class Cycle:
    def __init__(self, out):
        self.out = out
        self.tftp = ROOT / 'tftpboot'
        self.backup = self.tftp / '.fbsd-backup'
        self.micro = ROOT / 'microkernel'
        self.ssh = ['ssh', '-i', '/root/.ssh/chimp_ed25519',
                    '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
                    'root@192.0.2.82']
        self.changed = False

    def log(self, message):
        line = time.strftime('%Y-%m-%dT%H:%M:%S%z ') + message
        print(line, flush=True)
        with (self.out / 'cycle.log').open('a') as dst:
            dst.write(line + '\n')

    def run(self, argv, name, timeout=30, check=True):
        try:
            result = subprocess.run(argv, cwd=self.micro, timeout=timeout,
                                    capture_output=True, env={**os.environ, 'LC_ALL': 'C'})
        except subprocess.TimeoutExpired as exc:
            (self.out / name).write_bytes(exc.stdout or b'')
            (self.out / (name + '.err')).write_bytes(exc.stderr or b'')
            raise
        (self.out / name).write_bytes(result.stdout)
        (self.out / (name + '.err')).write_bytes(result.stderr)
        if check and result.returncode:
            raise RuntimeError(f'{name}: exit {result.returncode}; see saved stderr')
        return result

    def guest(self, name, timeout=15):
        return self.run(self.ssh + [
            'test "$(uname -s)" = FreeBSD && '
            'mount -p | awk \'$2 == "/var" && $3 == "ufs" {ok=1} END {exit !ok}\' '
            '&& uname -r && uptime && mount -p'], name, timeout, check=False)

    def recover(self):
        self.log('Restore FreeBSD TFTP pairing')
        for name in FILES:
            shutil.copy2(self.backup / name, self.tftp / name)
            if md5(self.backup / name) != md5(self.tftp / name):
                raise RuntimeError(f'restore checksum mismatch: {name}')
        self.run(['python3', 'safemode.py', 'force', '0'], 'safemode-after.txt')
        self.run(['python3', '-c',
                  'from hvdbg import HV; import sys; '
                  'r=HV().wdt_reset(); print("wdt_reset ->",r); sys.exit(0 if r else 1)'],
                 'reset.txt', 120)
        deadline = time.monotonic() + 240
        attempt = 0
        while time.monotonic() < deadline:
            attempt += 1
            try:
                result = self.guest(f'guest-after-{attempt}.txt',
                                    min(15, max(1, deadline - time.monotonic())))
                if result.returncode == 0:
                    self.changed = False
                    self.log('FreeBSD SSH and UFS /var restored')
                    return
            except subprocess.TimeoutExpired:
                pass
            self.log(f'Waiting for FreeBSD: attempt {attempt}')
            time.sleep(5)
        raise RuntimeError('FreeBSD recovery deadline expired')

    def capture(self):
        if subprocess.run(['pgrep', '-f', r'chimpd\.py'], capture_output=True).returncode == 0:
            raise RuntimeError('chimpd.py is active; board ownership is not exclusive')
        for name in FILES:
            if md5(self.tftp / name) != md5(self.backup / name):
                raise RuntimeError(f'current TFTP differs from FreeBSD backup: {name}')
        if self.guest('guest-before.txt').returncode:
            raise RuntimeError('FreeBSD SSH/UFS preflight failed')
        self.run(['python3', 'triage.py'], 'triage-before.txt', 60)
        image = ROOT / 'linux-work/build-min/arch/arm64/boot/Image'
        staged = self.out / 'staged'
        staged.mkdir()
        with (ROOT / 'linux-work/build-min/.h264-campaign-build.lock').open('a') as build_lock:
            fcntl.flock(build_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            shutil.copy2(image, staged / 'kernel')
        shutil.copy2(self.backup / 'hv-linux.uimg', staged / 'microkernel-dbg.uimg')
        self.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(staged / 'bananapi-min.dtb'),
                  str(self.micro / 'linux-guest/bpi_m64_trace.dts')], 'dtc.txt')
        sources = [image, self.backup / 'hv-linux.uimg',
                   self.micro / 'linux-guest/bpi_m64_trace.dts',
                   ROOT / 'linux-work/build-min/.config',
                   ROOT / 'linux-work/initramfs-h264/init_h264.c',
                   ROOT / 'linux-work/initramfs-h264/h264s.h', Path(__file__).resolve()]
        manifest = {'sources': {str(p): md5(p) for p in sources},
                    'staged': {n: md5(staged / n) for n in FILES},
                    'freebsd': {n: md5(self.backup / n) for n in FILES}}
        for repo in ('bsdOS', 'fbsd-media', 'microkernel'):
            result = subprocess.run(['git', '-C', str(ROOT / repo), 'rev-parse', 'HEAD'],
                                    capture_output=True, text=True, check=True)
            manifest[repo] = result.stdout.strip()
        (self.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        self.run(['python3', 'safemode.py', 'force', '0'], 'safemode-before.txt')
        self.changed = True
        for name in FILES:
            shutil.copy2(staged / name, self.tftp / name)
            if md5(self.tftp / name) != manifest['staged'][name]:
                raise RuntimeError(f'stage checksum mismatch: {name}')
        self.log('Linux pairing staged; request guest reboot')
        # SSH may disconnect before shutdown returns. The console is the boot gate.
        try:
            self.run(self.ssh + ['shutdown -r now'], 'shutdown.txt', 20, check=False)
        except subprocess.TimeoutExpired:
            pass
        deadline = time.monotonic() + 240
        while time.monotonic() < deadline:
            time.sleep(5)
            try:
                result = self.run(['python3', 'vcon_dump.py', '40000'], 'console.txt',
                                  min(30, max(1, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                continue
            if b'TRACE-DONE' in result.stdout:
                break
            self.log('Waiting for rig completion')
        else:
            raise RuntimeError('TRACE-DONE deadline expired')
        self.run(['python3', 'vetrace.py'], 'trace.txt', 280)
        stats = (self.out / 'trace.txt.err').read_text()
        match = re.search(r'entries written (\d+), isv0 (\d+)', stats)
        rows = (self.out / 'trace.txt').read_text().splitlines()
        if not match or int(match[1]) != len(rows) or not rows or int(match[2]):
            raise RuntimeError('trace missing, incomplete, or has invalid entries')
        self.log(f'Captured {len(rows)} trace entries; decode comparison remains a separate gate')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', help='new artifact directory name (never overwritten)')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.tag):
        parser.error('invalid tag')
    base = ROOT / 'linux-work/h264-traces'
    with (base / '.board-cycle.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        out = base / args.tag
        out.mkdir()
        cycle = Cycle(out)
        def interrupted(signum, frame):
            raise RuntimeError(f'interrupted by signal {signum}')
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, interrupted)
        result = {'capture': False, 'recovered': False, 'decode_validated': False}
        try:
            cycle.capture()
            result['capture'] = True
        except Exception as exc:
            result['error'] = str(exc)
            cycle.log(f'FAIL: {exc}')
        finally:
            if cycle.changed:
                # Finish recovery even if the supervising shell sends another signal.
                for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                    signal.signal(sig, signal.SIG_IGN)
                try:
                    cycle.recover()
                    result['recovered'] = True
                except Exception as exc:
                    result['recovery_error'] = str(exc)
                    cycle.log(f'RECOVERY FAILED: {exc}')
            (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        return 0 if result['capture'] and result['recovered'] else 1


if __name__ == '__main__':
    sys.exit(main())
