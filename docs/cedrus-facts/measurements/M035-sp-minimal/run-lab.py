#!/usr/bin/env python3
"""Replay the measured requests serially; retrieve pixels and all guards."""
from pathlib import Path
import fcntl
import hashlib
import json
import re
import subprocess
import tarfile

B = Path(__file__).resolve().parent
G = 'root@192.0.2.82'
SSH = ['ssh', '-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes',
       '-o', 'ConnectTimeout=8']
SCP = ['scp', '-q'] + SSH[1:]
REMOTE = '/tmp/ve/m035-sp'
names = json.loads((B / 'stream-order.json').read_text())
jobs = [json.loads((B / name / 'normal/job.json').read_text()) for name in names]
assert all(not (B / name / 'normal/result.json').exists() for name in names)
with tarfile.open(B / 'lab-input.tar.gz', 'w:gz') as archive:
    for name, job in zip(names, jobs):
        for file in job['uploads']:
            path = Path(file)
            archive.add(path, arcname=f'{name}/{path.name}')
lines = ['#!/bin/sh', 'set -u']
for name in names:
    lines.append(f'(cd {name} && ../../ve_lab replay.lab > lab.txt 2>&1; '
                 'echo $? > lab-exit.txt)')
(B / 'guest-run.sh').write_text('\n'.join(lines) + '\n')
files = [B / 'lab-input.tar.gz', B / 'guest-run.sh']
expected = [hashlib.md5(p.read_bytes()).hexdigest() for p in files]
with (B.parent / '.board-cycle.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    subprocess.run(SSH + [G, f'mkdir {REMOTE}'], check=True, timeout=20)
    subprocess.run(SCP + [str(p) for p in files] + [G + ':' + REMOTE + '/'],
                   check=True, timeout=60)
    actual = subprocess.check_output(SSH + [G, f'cd {REMOTE} && md5 -q '
                                           'lab-input.tar.gz guest-run.sh'],
                                     timeout=30).decode().splitlines()
    assert actual == expected
    (B / 'lab-upload-md5.json').write_text(json.dumps(dict(zip(
        [p.name for p in files], expected)), indent=2) + '\n')
    p = subprocess.run(SSH + [G, f'cd {REMOTE} && tar -xzf lab-input.tar.gz && '
                             'sh guest-run.sh && tar -czf lab-output.tar.gz ' +
                             ' '.join(names)], capture_output=True, timeout=600)
    (B / 'lab-batch.txt').write_bytes(p.stdout + p.stderr)
    p.check_returncode()
    subprocess.run(SCP + [G + ':' + REMOTE + '/lab-output.tar.gz', str(B)],
                   check=True, timeout=60)
with tarfile.open(B / 'lab-output.tar.gz', 'r:gz') as archive:
    for name in names:
        out = B / name / 'normal'
        job = json.loads((out / 'job.json').read_text())
        for file in job['dumps'] + [f'guard-{i}.bin' for i in range(job['guards'])] + \
                ['lab.txt', 'lab-exit.txt']:
            (out / file).write_bytes(archive.extractfile(f'{name}/{file}').read())
results = []
for name, job in zip(names, jobs):
    out = B / name / 'normal'
    log = (out / 'lab.txt').read_bytes()
    registers = [(int(k, 16), int(v, 16)) for k, v in
                 re.findall(rb'^([0-9a-fA-F]{3}) = ([0-9a-fA-F]+)$', log, re.M)]
    refs = [Path(job[key]).read_bytes() for key in ('reference', 'secondary_reference')]
    y = job['width'] * job['height']; n = y // 4; size = y + 2 * n
    frames = []
    for i, file in enumerate(job['dumps']):
        pixels = (out / file).read_bytes()
        exact = []
        differences = []
        for ref in refs:
            planar = ref[i * size:(i + 1) * size]
            uv = bytearray(2 * n)
            uv[0::2] = planar[y:y + n]; uv[1::2] = planar[y + n:]
            nv12 = planar[:y] + uv
            exact.append(pixels == nv12)
            differences.append(sum(a != z for a, z in zip(pixels, nv12)))
        frames.append(dict(frame=i, bytes=len(pixels), jm_exact=exact[0],
                           ffmpeg_exact=exact[1], different_bytes=differences))
    intact = all((out / f'guard-{i}.bin').read_bytes() == b'\xa5' * 4096
                 for i in range(job['guards']))
    report = dict(name=name, exit=int((out / 'lab-exit.txt').read_text()),
                  timeout=b'TIMEOUT' in log, registers=registers,
                  guards_intact=intact, guard_count=job['guards'], frames=frames)
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    results.append(report)
    print(json.dumps(report), flush=True)
(B / 'lab-results.json').write_text(json.dumps(results, indent=2) + '\n')
