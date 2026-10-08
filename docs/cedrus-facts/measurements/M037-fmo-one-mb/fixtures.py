#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Valid one-PCM-macroblock slices: raster control and interleaved FMO.

The host constructs test input only. Every output sample is written by VE.
No decoder or output reordering is implemented here.
"""
from pathlib import Path
import json

B=Path(__file__).resolve().parent
W,H=352,288
class Bits:
    def __init__(self): self.bits=[]
    def u(self,x,n=1): self.bits.extend((x>>i)&1 for i in range(n-1,-1,-1))
    def ue(self,x): n=(x+1).bit_length();self.u(0,n-1);self.u(x+1,n)
    def se(self,x): self.ue(-2*x if x<=0 else 2*x-1)
    def align(self):
        while len(self.bits)%8:self.u(0)
    def tail(self):self.u(1);self.align()
    def data(self):
        assert not len(self.bits)%8
        return bytes(sum(self.bits[i+j]<<(7-j) for j in range(8))
                     for i in range(0,len(self.bits),8))
def nal(kind,data):
    result=bytearray(b'\0\0\0\1'+bytes([kind]));zeros=0
    for x in data:
        if zeros==2 and x<=3:result.append(3);zeros=0
        result.append(x);zeros=zeros+1 if x==0 else 0
    return bytes(result)
Y=bytes(16+(x*3+y*5)%220 for y in range(H) for x in range(W))
U=bytes(40+(x+3*y)%160 for y in range(H//2) for x in range(W//2))
V=bytes(50+(2*x+y)%160 for y in range(H//2) for x in range(W//2))
s=Bits();s.u(88,8);s.u(0,8);s.u(30,8);s.ue(0);s.ue(0);s.ue(2);s.ue(1);s.u(0)
s.ue(21);s.ue(17);s.u(1);s.u(1);s.u(0);s.u(0);s.tail()
template=(B.parent/'m033-fmo/contiguous/probe.lab').read_text().splitlines()
starts=[i for i,line in enumerate(template) if line.startswith('echo')]
block=template[starts[0]+1:starts[1]]
split=next(i for i,line in enumerate(block) if line.startswith('wr 0x238'))
finish=next(i for i,line in enumerate(block) if line.startswith('wr 0x204'))
setup=block[:split]
tail=block[finish:]
for name,fmo in [('raster',False),('fmo',True)]:
    d=B/name;d.mkdir(exist_ok=True)
    p=Bits();p.ue(0);p.ue(0);p.u(0);p.u(0);p.ue(1 if fmo else 0)
    if fmo:p.ue(0);p.ue(0);p.ue(0)
    p.ue(0);p.ue(0);p.u(0);p.u(0,2);p.se(0);p.se(0);p.se(0);p.u(1);p.u(0);p.u(0);p.tail()
    stream=bytearray(nal(0x67,s.data())+nal(0x68,p.data()))
    packed=bytearray();params=[]
    order=list(range(0,396,2))+list(range(1,396,2)) if fmo else list(range(396))
    lines=['reset','fill 0 0x800000 0','load input.bin 0',
           'fill 0x1ff000 0x1000 0xa5','fill 0x200000 0x100000 0x5a',
           'fill 0x300000 0x1000 0xa5','fill 0x3ff000 0x1000 0xa5',
           'fill 0x408000 0x1000 0xa5','sync']
    for index,mb in enumerate(order):
        my,mx=divmod(mb,22)
        sl=Bits();sl.ue(mb);sl.ue(2);sl.ue(0);sl.u(0,4);sl.ue(0)
        sl.u(0);sl.u(0);sl.se(0);sl.ue(1);header=len(sl.bits)+40
        sl.ue(25);sl.align()
        for plane,pw,bw,bh in [(Y,W,16,16),(U,W//2,8,8),(V,W//2,8,8)]:
            for y in range(my*bh,(my+1)*bh):
                for x in range(mx*bw,(mx+1)*bw):sl.u(plane[y*pw+x],8)
        sl.tail();data=nal(0x65,sl.data());stream+=data
        offset=len(packed);packed+=data;packed+=bytes((-len(packed))%512)
        params.append(dict(first_mb=mb,offset=offset,bytes=len(data),header_bits=header))
        lines.append(f'echo slice {index} first_mb {mb}')
        lines+=setup
        lines += [f'wr 0x238 {len(data)*8}','wr 0x234 0',
                  f'wrpa 0x23c {offset+len(data):#x}',f'wrvld 0x230 {offset:#x} 7',
                  'wr 0x50 0','wr 0x224 7']
        remaining=header
        while remaining:
            bits=min(remaining,32);remaining-=bits
            lines += [f'wr 0x224 {(bits<<8)|3:#x}','poll 0x228 0x100 0 200']
        current=(mx<<24)|(my<<16)|0x1200|(0x20 if index==0 else 0)
        lines += [f'wr 0x208 {current:#x}' if x.startswith('wr 0x208 ') else x for x in tail]
    lines += ['sync','dump 0x200000 0x100000 output.bin',
              'dump 0x1ff000 0x1000 output-before.bin','dump 0x300000 0x1000 output-after.bin',
              'dump 0x3ff000 0x1000 mv-before.bin','dump 0x408000 0x1000 mv-after.bin']
    (d/'stream.264').write_bytes(stream);(d/'input.bin').write_bytes(packed)
    (d/'reference.yuv').write_bytes(Y+U+V)
    (d/'params.json').write_text(json.dumps(params,indent=2)+'\n')
    (d/'probe.lab').write_text('\n'.join(lines)+'\n')
    print(name,len(stream),len(packed),len(lines))
