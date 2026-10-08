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
for c in [0,1,2,3]:
 d=b/f'pcm{c}';d.mkdir(exist_ok=True)
 s=Bits();s.u(122 if c==2 else 244 if c==3 else 100,8);s.u(0,8);s.u(30,8);s.ue(0);s.ue(c)
 if c==3:s.u(0)
 s.ue(0);s.ue(0);s.u(0);s.u(0);s.ue(0);s.ue(2);s.ue(1);s.u(0);s.ue(W//16-1);s.ue(H//16-1);s.u(1);s.u(1);s.u(0);s.u(0);s.tail()
 p=Bits();p.ue(0);p.ue(0);p.u(0);p.u(0);p.ue(0);p.ue(0);p.ue(0);p.u(0);p.u(0,2);p.se(0);p.se(0);p.se(0);p.u(1);p.u(0);p.u(0);p.tail()
 sl=Bits();sl.ue(0);sl.ue(2);sl.ue(0);sl.u(0,4);sl.ue(0);sl.u(0);sl.u(0);sl.se(0);sl.ue(1)
 Y=bytes(16+(x*3+y*5)%220 for y in range(H) for x in range(W));sx=1 if c==3 else 2;sy=2 if c==1 else 1
 CW,CH=(W//sx,H//sy) if c else (0,0)
 U=bytes(40+(x+3*y)%160 for y in range(CH) for x in range(CW));V=bytes(50+(2*x+y)%160 for y in range(CH) for x in range(CW))
 for my in range(H//16):
  for mx in range(W//16):
   sl.ue(25);sl.align()
   for plane,pw,bw,bh in [(Y,W,16,16)]+([(U,CW,16//sx,16//sy),(V,CW,16//sx,16//sy)] if c else []):
    for y in range(my*bh,(my+1)*bh):
     for x in range(mx*bw,(mx+1)*bw):sl.u(plane[y*pw+x],8)
 sl.tail();(d/'stream.264').write_bytes(nal(0x67,s.data())+nal(0x68,p.data())+nal(0x65,sl.data()));expected=Y+U+V if c else Y+bytes([128])*(W*H//2);(d/'reference.yuv').write_bytes(expected)
 fmt={0:'yuv420p',1:'yuv420p',2:'yuv422p',3:'yuv444p'}[c]
 r=subprocess.run(['ffmpeg','-v','error','-threads','2','-i',str(d/'stream.264'),'-pix_fmt',fmt,'-f','rawvideo','pipe:1'],capture_output=True);assert r.returncode==0 and not r.stderr and r.stdout==expected,(c,len(r.stdout),len(expected),r.stderr)
 with (d/'params.json').open('w') as f:subprocess.run([str(b.parent/'m027-mono/export'),str(d/'stream.264'),str(d/'input.bin')],stdout=f,check=True)
 print(c,'ffmpeg exact',len(expected),json.loads((d/'params.json').read_text()))
