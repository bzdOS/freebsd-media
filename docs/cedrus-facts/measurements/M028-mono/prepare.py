#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""First-I-picture experiment: measured 352x288 replay + our parsed controls.
Only the two generated variants' H264_PICSIZE bit 19 differs. This is a probe,
not a claim of monochrome support. No address/clock/reset encoding is guessed.
"""
import json,struct
from pathlib import Path
b=Path(__file__).resolve().parent
old=(b.parent/'m027-lt-lab/replay.lab').read_text().splitlines()
first=old[old.index('echo slice 0 picture 0'):old.index('echo slice 1 picture 1')]
for name in ['HPCAMOLQ_BRCM_B','HPCVMOLQ_BRCM_B','FRExt_MMCO4_Sony_B']:
 out=b/name;p=json.loads((out/'params.json').read_text());sm=(out/'input.bin.scaling').read_bytes()
 assert (p['wmb'],p['hmb'],p['sps_flags'],p['depth'])==(22,18,80,0) and p['chroma'] in (0,1)
 assert len(sm)>=224 and p['scaling'] and p['bytes']<0x100000
 for chroma in [1,0]:
  lines=['reset','fill 0 0x800000 0','load input.bin 0','fill 0x218c00 0xc600 0x80','fill 0x225200 0xe00 0xa5','sync']
  ptr=None;skip_seen=False
  for line in first:
   t=line.split()
   if len(t)==3 and t[0]=='wr' and int(t[1],0)==0x2e0:ptr=int(t[2],0)
   elif len(t)==3 and t[0] in ['wr','wrpa'] and int(t[1],0)==0x2e4:
    if 0x800<=ptr<0x880:
     offset=96+ptr-0x800;line=f'wr 0x2e4 {struct.unpack_from("<I",sm,offset)[0]:#x}'
    elif 0x880<=ptr<0x8e0:
     offset=ptr-0x880;line=f'wr 0x2e4 {struct.unpack_from("<I",sm,offset)[0]:#x}'
    ptr+=4
   if line.startswith('wr 0x238 '):line=f'wr 0x238 {p["bytes"]*8}'
   elif line.startswith('wrpa 0x23c '):line=f'wrpa 0x23c {p["bytes"]:#x}'
   elif line.startswith('wr 0x224 ') and int(line.split()[2],0)&0xff==3:
    if not skip_seen:
     remaining=p['header_bits']
     while remaining:
      n=min(32,remaining);lines.extend([f'wr 0x224 {(n<<8)|3:#x}','poll 0x228 0x100 0 200']);remaining-=n
     skip_seen=True
    continue
   elif line=='poll 0x228 0x100 0 200':continue
   elif line.startswith('wr 0x204 '):line=f'wr 0x204 {p["flags_reg"]:#x}'
   elif line.startswith('wr 0x200 '):
    assert int(line.split()[2],0)==0xd1511
    line=f'wr 0x200 {0x51511|(chroma<<19):#x}'
   elif line.startswith('wr 0x208 '):line=f'wr 0x208 {p["slice_reg"]|0x20:#x}'
   elif line.startswith('wr 0x20c '):line=f'wr 0x20c {p["dbf_reg"]:#x}'
   elif line.startswith('wr 0x21c '):line=f'wr 0x21c {p["qp_reg"]:#x}'
   elif line.startswith('wr 0x228 ') and int(line.split()[2],0)!=7:line='wr 0x228 0x7'
   lines.append(line)
  lines+=['sync','dump 0x22a000 0x1000 guard.bin','dump 0x225200 0xe00 output-guard.bin']
  (out/f'chroma-{chroma}.lab').write_text('\n'.join(lines)+'\n')
 a=(out/'chroma-0.lab').read_text().splitlines();d=(out/'chroma-1.lab').read_text().splitlines();diff=[(x,y) for x,y in zip(a,d) if x!=y];assert len(diff)==1
 print(name,'variant difference',diff)
