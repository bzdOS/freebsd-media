#!/usr/bin/env python3
"""Restore the ordinary module and verify full Y/C after research jobs."""
from pathlib import Path
import fcntl
import hashlib
import json
import re
import subprocess

B = Path(__file__).resolve().parent
W = Path('/opt/bzdos')
SSH = ['ssh', '-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes',
       '-o', 'ConnectTimeout=8']
SCP = ['scp', '-q'] + SSH[1:]
G = 'root@192.0.2.82'
cases = [
    ('f3', W / 'bsdOS/docs/cedrus-facts/vectors/h264/streams/f3.264', 8),
    ('gray-weighted', W / 'linux-work/h264-traces/m027-mono/gray-weighted.264', 36),
    ('gray-mbaff-cabac1', W / 'linux-work/h264-traces/m027-mono/gray-mbaff-cabac1.264', 36),
]
rows = []
with (B.parent / '.board-cycle.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    actual = subprocess.check_output(SSH + [G, 'md5 -q /tmp/cedrus.ko'],
                                    timeout=20).decode().strip()
    expected = hashlib.md5((W / 'fbsd-obj/media/cedrus/cedrus.ko').read_bytes()).hexdigest()
    assert actual == expected == 'd57627e64bb4cfc21e64d264e9a19b5a'
    subprocess.run(SSH + [G, 'kldunload cedrus && kldload /tmp/cedrus.ko'],
                   check=True, timeout=30)
    for name, stream, count in cases:
        reference = B.parent / 'm034-depth' / (name + '.yuv')
        paths = [stream, reference]
        subprocess.run(SCP + [str(p) for p in paths] + [G + ':/tmp/ve/'],
                       check=True, timeout=60)
        checksums = [hashlib.md5(p.read_bytes()).hexdigest() for p in paths]
        remote = ' '.join('/tmp/ve/' + p.name for p in paths)
        actual = subprocess.check_output(SSH + [G, 'md5 -q ' + remote],
                                        timeout=30).decode().splitlines()
        assert actual == checksums
        p = subprocess.run(SSH + [G, '/tmp/ve/cedrus_h264 ' + remote],
                           capture_output=True, timeout=180)
        output = p.stdout + p.stderr
        (B / (name + '-recovery.txt')).write_bytes(output)
        passes = len(re.findall(rb'^picture .*: PASS  Y ', output, re.M))
        assert p.returncode == 0 and passes == count
        assert not re.search(rb'FAIL|TIMEOUT|ioctl:', output)
        rows.append(dict(stream=name, frames=passes, exit=p.returncode,
                         upload_md5=checksums))
        print(name, passes, 'full Y/C exact', flush=True)
    command = ('sysctl dev.cedrus.0; vmstat -m | egrep "vbq|cedrus"; '
               'md5 -q /tmp/cedrus.ko; uptime')
    p = subprocess.run(SSH + [G, command], capture_output=True, timeout=30)
    p.check_returncode()
    (B / 'final-state.txt').write_bytes(p.stdout + p.stderr)
    state = p.stdout.decode()
    for field, value in [('pictures', 80), ('errors', 0), ('timeouts', 0),
                         ('rejects', 0), ('missing_refs', 0)]:
        assert f'dev.cedrus.0.{field}: {value}\n' in state
    assert re.search(r'^\s*vbq\s+0\s+0\s', state, re.M)
    (B / 'recovery.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(state, flush=True)
