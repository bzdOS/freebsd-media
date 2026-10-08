#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Install separately fetched public API headers and build the research host."""
from pathlib import Path
import fcntl
import hashlib
import json
import subprocess
import tarfile

B = Path(__file__).resolve().parent
W = Path('/opt/bzdos')
SSH = ['ssh', '-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes',
       '-o', 'ConnectTimeout=8']
SCP = ['scp', '-q'] + SSH[1:]
G = 'root@192.0.2.82'
R = '/tmp/ve/cedar-probe'
entries = json.loads((B / 'sdk-provenance.json').read_text())
for entry in entries:
    data = (B / entry['local']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == entry['sha256']
    assert hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() == entry['git_blob_sha1']
files = [B / e['local'] for e in entries if e['local'].endswith('.h')]
files += [B / 'cedar-probe.c', B / 'elf-loader.h',
          W / 'fbsd-media/sys/dev/cedrus/cedrus_lab.h']
archive = B / 'prepare-source.tar.gz'
with tarfile.open(archive, 'w:gz') as out:
    for path in files:
        out.add(path, arcname=path.name)
digest = hashlib.md5(archive.read_bytes()).hexdigest()
with (W / 'linux-work/h264-traces/.board-cycle.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    subprocess.run(SSH + [G, f'mkdir -p {R}'], check=True, timeout=20)
    subprocess.run(SCP + [str(archive), G + ':' + R + '/'], check=True, timeout=60)
    actual = subprocess.check_output(SSH + [G, f'md5 -q {R}/{archive.name}'],
                                     timeout=20).decode().strip()
    assert actual == digest
    command = (f'cd {R} && tar -xzf {archive.name} && LC_ALL=C '
               'cc -O1 -g -Wall -I. -o cedar-probe cedar-probe.c')
    p = subprocess.run(SSH + [G, command], capture_output=True, timeout=90)
    (B / 'prepare-build.txt').write_bytes(p.stdout + p.stderr)
    (B / 'prepare.json').write_text(json.dumps(dict(
        command=command, exit=p.returncode, archive_md5=digest,
        sources={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
    ), indent=2) + '\n')
    p.check_returncode()
    print('Public APIs verified; research host compiled, exit', p.returncode)
