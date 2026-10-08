#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import json, subprocess, hashlib, os, sys
from concurrent.futures import ThreadPoolExecutor
R='/opt/bzdos/linux-work/jvt'
c=json.load(open(R+'/catalog.json'))
def md5file(p):
    h=hashlib.md5()
    with open(p,'rb') as f:
        while True:
            b=f.read(1<<22)
            if not b: break
            h.update(b)
    return h.hexdigest()
def work(item):
    n,e=item
    if 'bitstream' not in e or e.get('ffmpeg_md5'): return n,e
    pix=e.get('pix_fmt') or 'yuv420p'
    r=subprocess.run(['ffmpeg','-v','error','-i',e['bitstream'],'-f','rawvideo','-pix_fmt',pix,'-'],capture_output=True)
    e['ffmpeg_ok']=r.returncode==0 and len(r.stdout)>0
    e['ffmpeg_md5']=hashlib.md5(r.stdout).hexdigest(); e['ffmpeg_bytes']=len(r.stdout)
    e['ffmpeg_err']=r.stderr.decode(errors='replace')[:200]
    e['provided']=[]
    for y in e.get('yuv',[]):
        try: e['provided'].append({'file':y,'bytes':os.path.getsize(y),'md5':md5file(y)})
        except Exception as ex: pass
    e['ref_match']=any(p['md5']==e['ffmpeg_md5'] for p in e['provided'])
    return n,e
with ThreadPoolExecutor(6) as ex:
    for n,e in ex.map(work,list(c.items())):
        c[n]=e
json.dump(c,open(R+'/catalog.json','w'),indent=1)
ok=sum(1 for e in c.values() if e.get('ref_match')); print('ref_match',ok,'of',len(c))
for n,e in sorted(c.items()):
    if 'bitstream' in e and not e.get('ref_match'): print('  nomatch',n,e.get('profile'),e.get('ffmpeg_ok'),e.get('ffmpeg_bytes'),[p['bytes'] for p in e.get('provided',[])],e.get('ffmpeg_err','')[:60].replace('\n',' '))
