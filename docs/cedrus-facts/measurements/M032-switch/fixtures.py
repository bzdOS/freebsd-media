# SPDX-License-Identifier: BSD-2-Clause
"""Locally constructed single-picture H.264 I_PCM probes, not decoder code."""
from pathlib import Path
import subprocess,json
b=Path(__file__).resolve().parent
class Bits:
 def __init__(self):self.bits=[]
 def u(self,x,n=1):self.bits.extend((x>>i)&1 for i in range(n-1,-1,-1))
 def ue(self,x):n=(x+1).bit_length();self.u(0,n-1);self.u(x+1,n)
 def se(self,x):self.ue(-2*x if x<=0 else 2*x-1)
 def align(self):
  while len(self.bits)%8:self.u(0)
 def tail(self):self.u(1);self.align()
 def data(self):
  assert not len(self.bits)%8
  return bytes(sum(self.bits[i+j]<<(7-j) for j in range(8)) for i in range(0,len(self.bits),8))
def nal(typ,payload):
 out=bytearray(b'\0\0\0\1'+bytes([typ]));zeros=0
 for x in payload:
  if zeros==2 and x<=3:out.append(3);zeros=0
  out.append(x);zeros=zeros+1 if x==0 else 0
 return out
W,H=352,288
Y=bytes(16+(x*3+y*5)%220 for y in range(H) for x in range(W));U=bytes(40+(x+3*y)%160 for y in range(H//2) for x in range(W//2));V=bytes(50+(2*x+y)%160 for y in range(H//2) for x in range(W//2))
s=Bits();s.u(88,8);s.u(0,8);s.u(30,8);s.ue(0);s.ue(0);s.ue(2);s.ue(1);s.u(0);s.ue(W//16-1);s.ue(H//16-1);s.u(1);s.u(1);s.u(0);s.u(0);s.tail()
p=Bits();p.ue(0);p.ue(0);p.u(0);p.u(0);p.ue(0);p.ue(0);p.ue(0);p.u(0);p.u(0,2);p.se(0);p.se(0);p.se(0);p.u(1);p.u(0);p.u(0);p.tail()
def picture(typ,idr):
 sl=Bits();sl.ue(0);sl.ue(typ);sl.ue(0);sl.u(0 if idr else 1,4)
 if idr:sl.ue(0)
 if typ in (0,3):sl.u(0);sl.u(0)
 if idr:sl.u(0);sl.u(0)
 else:sl.u(0)
 sl.se(0)
 if typ==3:sl.u(0)
 if typ in (3,4):sl.se(0)
 sl.ue(1);header_bits=len(sl.bits)+40
 for my in range(H//16):
  for mx in range(W//16):
   if typ in (0,3):sl.ue(0)
   sl.ue(30 if typ in (0,3) else 26 if typ==4 else 25);sl.align()
   for plane,pw,bw,bh in [(Y,W,16,16),(U,W//2,8,8),(V,W//2,8,8)]:
    for y in range(my*bh,(my+1)*bh):
     for x in range(mx*bw,(mx+1)*bw):sl.u(plane[y*pw+x],8)
 sl.tail();return nal(0x65 if idr else 0x41,sl.data()),header_bits
idr,_=picture(2,True)
for name,typ in [('i',2),('p',0),('sp',3),('si',4)]:
 d=b/name;d.mkdir(exist_ok=True);data,hdr=picture(typ,False)
 (d/'stream.264').write_bytes(nal(0x67,s.data())+nal(0x68,p.data())+idr+data)
 (d/'input.bin').write_bytes(data);(d/'reference.yuv').write_bytes(Y+U+V)
 (d/'params.json').write_text(json.dumps(dict(slice_type=typ,header_bits=hdr,bytes=len(data)))+'\n')
 print(name,hdr,len(data))
