#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""MD5-verified delivery, exclusive lab ownership and capture of vendor probes."""
from pathlib import Path
import fcntl
import hashlib
import json
import re
import subprocess
import sys
import tarfile

B = Path(__file__).resolve().parent
name = sys.argv[1]
stream = Path(sys.argv[2]).resolve()
library = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else B / 'android7-lib64/libawh264.so'
assert re.fullmatch(r'[a-z0-9-]+', name)
out = B / name
out.mkdir(exist_ok=True)
assert not (out / 'probe.txt').exists() and not (out / 'run.json').exists()
ssh = ['ssh', '-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes',
       '-o', 'ConnectTimeout=8']
scp = ['scp', '-q'] + ssh[1:]
guest = 'root@192.0.2.82'
root = '/tmp/ve/cedar-probe'
with Path('/opt/bzdos/linux-work/h264-traces/.board-cycle.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    subprocess.run(ssh + [guest, f'mkdir -p {root}/{name}'], check=True, timeout=20)
    files = [B / 'cedar-probe.c', B / 'elf-loader.h', stream, library]
    destinations = [root + '/cedar-probe.c', root + '/elf-loader.h',
                    root + '/' + name + '/stream.264', root + '/' + name + '/libawh264.so']
    checksums = {}
    for file, dest in zip(files, destinations):
        subprocess.run(scp + [str(file), guest + ':' + dest], check=True, timeout=60)
        actual = subprocess.check_output(ssh + [guest, 'md5 -q ' + dest],
                                         timeout=20).decode().strip()
        expected = hashlib.md5(file.read_bytes()).hexdigest()
        assert actual == expected
        checksums[str(file)] = expected
    (out / 'upload-md5.json').write_text(json.dumps(checksums, indent=2) + '\n')
    command = (f'cd {root} && LC_ALL=C cc -O1 -g -Wall -I. -o cedar-probe '
               f'cedar-probe.c && cd {name} && ../cedar-probe libawh264.so stream.264')
    p = subprocess.run(ssh + [guest, command], capture_output=True, timeout=150)
    (out / 'probe.txt').write_bytes(p.stdout + p.stderr)
    (out / 'run.json').write_text(json.dumps(dict(name=name, stream=str(stream),
        exit=p.returncode, command=command), indent=2) + '\n')
    subprocess.run(ssh + [guest, f'cd {root} && tar -czf {name}.tar.gz {name}'],
                   check=True, timeout=30)
    subprocess.run(scp + [guest + ':' + root + '/' + name + '.tar.gz', str(out)],
                   check=True, timeout=60)
with tarfile.open(out / (name + '.tar.gz')) as archive:
    for member in archive:
        if member.isfile():
            (out / Path(member.name).name).write_bytes(archive.extractfile(member).read())
print(p.stdout.decode() + p.stderr.decode(), flush=True)
print('exit', p.returncode, flush=True)
