#!/usr/bin/env python3
import fcntl,hashlib,json,re,subprocess,sys
from pathlib import Path
b=Path(__file__).resolve().parent
name,variant=sys.argv[1:];assert name in ('jm-p','jm-sp26','jm-sp36') and (variant in ('normal','as-p','qp-as-qs') or re.fullmatch(r'type-(?:[0-9]|1[0-5])(?:-qs)?',variant))
d=b/name/variant;j=json.loads((d/'job.json').read_text());assert not (d/'result.json').exists()
s=['ssh','-i','/root/.ssh/chimp_ed25519','-o','BatchMode=yes','-o','ConnectTimeout=8'];g='root@192.0.2.82';cp=['scp','-q']+s[1:];remote=f'/tmp/ve/m032-{name}-{variant}'
with (b.parent/'.board-cycle.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 subprocess.run(s+[g,f'mkdir -p {remote}'],check=True,timeout=20)
 subprocess.run(cp+j['uploads']+[g+':'+remote+'/'],check=True,timeout=60)
 expected=[hashlib.md5(Path(p).read_bytes()).hexdigest() for p in j['uploads']]
 actual=subprocess.check_output(s+[g,f'cd {remote} && md5 -q '+' '.join(Path(p).name for p in j['uploads'])],timeout=30).decode().splitlines();assert expected==actual
 (d/'upload-md5.json').write_text(json.dumps(dict(zip(j['uploads'],expected)),indent=2)+'\n')
 p=subprocess.run(s+[g,f'cd {remote} && ../ve_lab replay.lab'],capture_output=True,timeout=45);(d/'lab.txt').write_bytes(p.stdout+p.stderr)
 files=j['dumps']+[f'guard-{i}.bin' for i in range(j['guards'])]
 for f in files:subprocess.run(cp+[g+':'+remote+'/'+f,str(d/f)],check=True,timeout=30)
 registers=[(int(k,16),int(v,16)) for k,v in re.findall(rb'^([0-9a-fA-F]{3}) = ([0-9a-fA-F]+)$',p.stdout,re.M)]
 reference=Path(j['reference']).read_bytes();frames=[];size=38016;y=25344;n=6336
 for i,f in enumerate(j['dumps']):
  r=reference[i*size:(i+1)*size];uv=bytearray(n*2);uv[0::2]=r[y:y+n];uv[1::2]=r[y+n:];nv=r[:y]+uv;data=(d/f).read_bytes();frames.append(dict(frame=i,encoder_recon_exact=data==nv,matching_bytes=sum(a==z for a,z in zip(data,nv)),bytes=len(data)))
 guards=all((d/f'guard-{i}.bin').read_bytes()==b'\xa5'*4096 for i in range(j['guards']))
 result=dict(name=name,variant=variant,exit=p.returncode,registers=registers,guards_intact=guards,guard_count=j['guards'],frames=frames)
 (d/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
