#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import fcntl,hashlib,json,re,subprocess,sys
from pathlib import Path
base=Path(__file__).resolve().parent
name=sys.argv[1];chroma=int(sys.argv[2]);assert name in ['HPCAMOLQ_BRCM_B','HPCVMOLQ_BRCM_B','FRExt_MMCO4_Sony_B'] and chroma in (0,1)
suffix=sys.argv[3] if len(sys.argv)>3 else '';assert re.fullmatch(r'[-a-z0-9]*',suffix)
dataset=base/name;out=dataset/f'run-{chroma}{suffix}';out.mkdir()
files=[dataset/'input.bin',dataset/f'chroma-{chroma}{suffix}.lab']
ssh=['ssh','-i','/root/.ssh/chimp_ed25519','-o','BatchMode=yes','-o','ConnectTimeout=8'];scp=['scp','-q']+ssh[1:];guest='root@192.0.2.82';gd=f'/tmp/ve/mono-{name}-{chroma}{suffix}'
with (base.parent/'.board-cycle.lock').open('a') as lock:
 fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
 subprocess.run(ssh+[guest,f'mkdir {gd}'],check=True,timeout=20)
 subprocess.run(scp+[str(p) for p in files]+[guest+':'+gd+'/'],check=True,timeout=30)
 actual=subprocess.check_output(ssh+[guest,f'cd {gd} && md5 -q '+' '.join(p.name for p in files)],timeout=20).decode().splitlines()
 expected=[hashlib.md5(p.read_bytes()).hexdigest() for p in files];assert actual==expected
 (out/'upload-md5.json').write_text(json.dumps(dict(zip([p.name for p in files],expected)),indent=2)+'\n')
 r=subprocess.run(ssh+[guest,f'cd {gd} && ../ve_lab chroma-{chroma}{suffix}.lab'],capture_output=True,timeout=25)
 (out/'lab.txt').write_bytes(r.stdout+r.stderr)
 statuses=[int(v,16) for v in re.findall(rb'^228 = ([0-9a-fA-F]+)$',r.stdout,re.M)]
 complete=r.returncode==0 and b'TIMEOUT' not in r.stdout and len(statuses)==1 and statuses[0]&7==1
 # A failed probe must not be promoted to a decode result.
 if not complete:
  report=dict(name=name,chroma=chroma,complete=False,exit=r.returncode,statuses=statuses)
 else:
  subprocess.run(scp+[guest+':'+gd+'/frame-0.nv12',guest+':'+gd+'/guard.bin',guest+':'+gd+'/output-guard.bin',str(out)+'/'],check=True,timeout=30)
  catalog=json.loads((base.parents[1]/'jvt/catalog.json').read_text())
  ref=subprocess.check_output(['ffmpeg','-v','error','-threads','2','-apply_cropping','0','-i',catalog[name]['bitstream'],'-frames:v','1','-pix_fmt','nv12','-f','rawvideo','pipe:1'])
  data=(out/'frame-0.nv12').read_bytes();assert len(data)==len(ref)==352*288*3//2
  report=dict(name=name,chroma=chroma,complete=True,exact=data==ref,
   y_exact=data[:352*288]==ref[:352*288],uv_exact=data[352*288:]==ref[352*288:],
   different_bytes=sum(a!=b for a,b in zip(data,ref)),
   mv_guard=(out/'guard.bin').read_bytes()==b'\xa5'*4096,
   output_guard=(out/'output-guard.bin').read_bytes()==b'\xa5'*0xe00,
   statuses=statuses)
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
