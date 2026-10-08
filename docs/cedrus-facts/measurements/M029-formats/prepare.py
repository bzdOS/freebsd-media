#!/usr/bin/env python3
"""Bounded candidate: chroma_format_idc in PICSIZE bits19..20, not established.
No production capability follows from completion or a Y-only match.
Output reservation is 1 MiB (including canaries) before a separate MV arena.
"""
import json,struct
from pathlib import Path
b=Path(__file__).resolve().parent
old=(b.parent/'m027-lt-lab/replay.lab').read_text().splitlines()
first=old[old.index('echo slice 0 picture 0'):old.index('echo slice 1 picture 1')]
for d in sorted(b.glob('yuv*-cabac*')):
 p=json.loads((d/'params.json').read_text());sm=(d/'input.bin.scaling').read_bytes()
 assert (p['wmb'],p['hmb'],p['sps_flags'],p['depth'])==(22,18,80,0)
 # 420 positive controls and a 420/actual-chroma comparison of identical others.
 for chroma in sorted({1,p['chroma']}):
  lines=['reset','fill 0 0x800000 0','load input.bin 0',
    'fill 0x1ff000 0x1000 0xa5','fill 0x200000 0x100000 0x5a',
    'fill 0x300000 0x1000 0xa5','fill 0x3ff000 0x1000 0xa5',
    'fill 0x408000 0x1000 0xa5','sync']
  ptr=None;skip=False
  for line in first:
   t=line.split()
   if len(t)==3 and t[0]=='wr' and int(t[1],0)==0x2e0:ptr=int(t[2],0)
   elif len(t)==3 and t[0] in ('wr','wrpa') and int(t[1],0)==0x2e4:
    if 0x800<=ptr<0x880:line=f'wr 0x2e4 {struct.unpack_from("<I",sm,96+ptr-0x800)[0]:#x}'
    elif 0x880<=ptr<0x8e0:line=f'wr 0x2e4 {struct.unpack_from("<I",sm,ptr-0x880)[0]:#x}'
    ptr+=4
   if line.startswith('fill ') or line=='sync':continue
   if line=='wrpa 0x2e4 0x226000':line='wrpa 0x2e4 0x400000'
   elif line=='wrpa 0x2e4 0x2278c0':line='wrpa 0x2e4 0x4018c0'
   elif line.startswith('wr 0x238 '):line=f'wr 0x238 {p["bytes"]*8}'
   elif line.startswith('wrpa 0x23c '):line=f'wrpa 0x23c {p["bytes"]:#x}'
   elif line.startswith('wr 0x224 ') and int(t[2],0)&0xff==3:
    if not skip:
     remaining=p['header_bits']
     while remaining:
      n=min(32,remaining);lines.extend([f'wr 0x224 {(n<<8)|3:#x}','poll 0x228 0x100 0 200']);remaining-=n
     skip=True
    continue
   elif line=='poll 0x228 0x100 0 200':continue
   elif line.startswith('wr 0x204 '):line=f'wr 0x204 {p["flags_reg"]:#x}'
   elif line.startswith('wr 0x200 '):line=f'wr 0x200 {0x51511|(chroma<<19):#x}'
   elif line.startswith('wr 0x208 '):line=f'wr 0x208 {p["slice_reg"]|0x20:#x}'
   elif line.startswith('wr 0x20c '):line=f'wr 0x20c {p["dbf_reg"]:#x}'
   elif line.startswith('wr 0x21c '):line=f'wr 0x21c {p["qp_reg"]:#x}'
   elif line.startswith('wr 0x228 ') and int(t[2],0)!=7:line='wr 0x228 0x7'
   elif line.startswith('dump '):continue
   lines.append(line)
  lines+=['sync','dump 0x200000 0x100000 output.bin',
    'dump 0x1ff000 0x1000 output-before.bin','dump 0x300000 0x1000 output-after.bin',
    'dump 0x3ff000 0x1000 mv-before.bin','dump 0x408000 0x1000 mv-after.bin']
  (d/f'chroma-{chroma}.lab').write_text('\n'.join(lines)+'\n')
  print(d.name,chroma,hex(0x51511|(chroma<<19)))
