#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import fcntl,hashlib,json,re,subprocess,sys
from pathlib import Path
b=Path(__file__).resolve().parent
name=sys.argv[1];chroma=int(sys.argv[2]);assert re.fullmatch(r'pcm[0-3]',name) and chroma in range(8)
d=b/name;out=d/f'run-{chroma}';out.mkdir()
files=[d/'input.bin',d/f'chroma-{chroma}.lab'];gdir=f'/tmp/ve/m030-{name}-{chroma}'
ssh=['ssh','-i','/root/.ssh/chimp_ed25519','-o','BatchMode=yes','-o','ConnectTimeout=8'];scp=['scp','-q']+ssh[1:];guest='root@192.0.2.82'
with (b.parent/'.board-cycle.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 subprocess.run(ssh+[guest,f'mkdir {gdir}'],check=True,timeout=20)
 subprocess.run(scp+[str(p) for p in files]+[guest+':'+gdir+'/'],check=True,timeout=30)
 actual=subprocess.check_output(ssh+[guest,f'cd {gdir} && md5 -q '+' '.join(p.name for p in files)],timeout=20).decode().splitlines()
 expected=[hashlib.md5(p.read_bytes()).hexdigest() for p in files];assert actual==expected
 (out/'upload-md5.json').write_text(json.dumps(dict(zip([p.name for p in files],expected)),indent=2)+'\n')
 r=subprocess.run(ssh+[guest,f'cd {gdir} && ../ve_lab chroma-{chroma}.lab'],capture_output=True,timeout=30)
 (out/'lab.txt').write_bytes(r.stdout+r.stderr)
 statuses=[int(x,16) for x in re.findall(rb'^228 = ([0-9a-fA-F]+)$',r.stdout,re.M)]
 complete=r.returncode==0 and b'TIMEOUT' not in r.stdout and len(statuses)==1 and statuses[0]&7==1
 registers={int(k,16):int(v,16) for k,v in re.findall(rb'^([0-9a-fA-F]{3}) = ([0-9a-fA-F]+)$',r.stdout,re.M)}
 report=dict(registers=registers,name=name,chroma_candidate=chroma,complete=complete,exit=r.returncode,statuses=statuses)
 # Collect guards even if VE declares an error; absence is a failed observation.
 for file in ['output.bin','output-before.bin','output-after.bin','mv-before.bin','mv-after.bin']:
  z=subprocess.run(scp+[guest+':'+gdir+'/'+file,str(out)+'/'+file],capture_output=True,timeout=30)
  if z.returncode:report[file+'_unavailable']=True
 guards={p.stem:p.read_bytes()==b'\xa5'*4096 for p in out.glob('*-*.bin')};report['guards']=guards
 if (out/'output.bin').exists():
  data=(out/'output.bin').read_bytes();ref=(d/'reference.yuv').read_bytes();Y=352*288;N=(len(ref)-Y)//2
  nv=bytearray(ref[:Y]);uv=bytearray(2*N);uv[0::2]=ref[Y:Y+N];uv[1::2]=ref[Y+N:];nv+=uv
  report.update(y_exact=data[:Y]==ref[:Y],candidate_interleaved_exact=data[:len(nv)]==nv,
   candidate_interleaved_different_bytes=sum(a!=c for a,c in zip(data,nv)),
   bytes_written_extent=max((i+1 for i,x in enumerate(data) if x!=0x5a),default=0),
   expected_bytes=len(ref),output_tail_unchanged=all(x==0x5a for x in data[len(ref):]))
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
