#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Known slice-start programming; one hardware job per PCM macroblock."""
from pathlib import Path
import fcntl
import hashlib
import json
import re
import subprocess
import tarfile

B=Path(__file__).resolve().parent
S=['ssh','-i','/root/.ssh/chimp_ed25519','-o','BatchMode=yes','-o','ConnectTimeout=8']
C=['scp','-q']+S[1:];G='root@192.0.2.82';remote='/tmp/ve/m037-fmo'
names=['raster','fmo']
assert all(x['exact'] and x['exit']==0 for x in json.load(open(B/'validation.json')))
assert all(not (B/n/'result.json').exists() for n in names)
with tarfile.open(B/'lab-input.tar.gz','w:gz') as a:
    for n in names:
        for f in ['input.bin','probe.lab']:a.add(B/n/f,arcname=f'{n}/{f}')
with (B.parent/'.board-cycle.lock').open('a') as lock:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    subprocess.run(S+[G,f'mkdir {remote}'],check=True,timeout=20)
    subprocess.run(C+[str(B/'lab-input.tar.gz'),G+':'+remote+'/'],check=True,timeout=60)
    md5=hashlib.md5((B/'lab-input.tar.gz').read_bytes()).hexdigest()
    actual=subprocess.check_output(S+[G,f'md5 -q {remote}/lab-input.tar.gz'],timeout=30).decode().strip();assert actual==md5
    (B/'upload-md5.json').write_text(json.dumps(dict(archive=md5))+'\n')
    command=f'cd {remote} && tar -xzf lab-input.tar.gz; '
    for n in names:command+=f'(cd {remote}/{n} && ../../ve_lab probe.lab > lab.txt 2>&1; echo $? > lab-exit.txt); '
    command+=f'cd {remote} && tar -czf lab-output.tar.gz raster fmo'
    p=subprocess.run(S+[G,command],capture_output=True,timeout=180);(B/'batch.txt').write_bytes(p.stdout+p.stderr);p.check_returncode()
    subprocess.run(C+[G+':'+remote+'/lab-output.tar.gz',str(B)],check=True,timeout=60)
with tarfile.open(B/'lab-output.tar.gz') as a:
    for n in names:
        for f in ['output.bin','output-before.bin','output-after.bin','mv-before.bin','mv-after.bin','lab.txt','lab-exit.txt']:
            (B/n/f).write_bytes(a.extractfile(n+'/'+f).read())
rows=[]
for n in names:
    d=B/n;log=(d/'lab.txt').read_bytes();statuses=[int(x,16) for x in re.findall(rb'^228 = ([0-9a-f]+)$',log,re.M)]
    params=json.load(open(d/'params.json'));ref=(d/'reference.yuv').read_bytes();size=352*288;nch=size//4;uv=bytearray(nch*2);uv[0::2]=ref[size:size+nch];uv[1::2]=ref[size+nch:];nv=ref[:size]+uv
    output=(d/'output.bin').read_bytes();guards={f:(d/(f+'.bin')).read_bytes()==b'\xa5'*4096 for f in ['output-before','output-after','mv-before','mv-after']}
    row=dict(name=n,slices=len(params),statuses=len(statuses),all_done=len(statuses)==len(params) and all(x&7==1 for x in statuses),timeout=b'TIMEOUT' in log,exit=int((d/'lab-exit.txt').read_text()),nv12_exact=output[:len(nv)]==nv,different_bytes=sum(a!=z for a,z in zip(output,nv)),guards=guards,output_tail_unchanged=all(x==0x5a for x in output[len(nv):]))
    (d/'result.json').write_text(json.dumps(row,indent=2)+'\n');rows.append(row);print(json.dumps(row),flush=True)
(B/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
