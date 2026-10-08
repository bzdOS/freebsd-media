# SPDX-License-Identifier: BSD-2-Clause
import sys, subprocess, os
name=sys.argv[1]
d=open(name+'.m2v','rb').read()
i=d.find(b'\x00\x00\x01\x01'); s=d[i:]
open('s_'+name+'.bin','wb').write(s)
N=len(s)
lab=open('/opt/bzdos/bsdOS/docs/cedrus-facts/vectors/e12-linux.lab').read()
lab=lab.replace('load s47.bin 0','load s_%s.bin 0'%name).replace('load i64s.bin 0','load s_%s.bin 0'%name)
lab=lab.replace('wr 0x130 17224','wr 0x130 %d'%(N*8)).replace('wrpa 0x134 2153','wrpa 0x134 %d'%N)
lab=lab.replace('wr 0x114 0x800000b8','wr 0x114 0x80000008')
lab+='dump 0x400000 0x10000 all_%s.bin\nrd 0x1c4\nrd 0x12c\n'%name
open('l_%s.lab'%name,'w').write(lab)
K=['-i','/root/.ssh/chimp_ed25519']; G='root@192.0.2.82'
subprocess.run(['scp','-q']+K+['s_%s.bin'%name,'l_%s.lab'%name,G+':/tmp/ve/'],check=True)
r=subprocess.run(['ssh']+K+[G,'cd /tmp/ve && ./ve_lab l_%s.lab 2>&1 | grep -E "^(11c|1c4|12c)"'%name],capture_output=True,text=True,stdin=subprocess.DEVNULL)
print(r.stdout.replace('\n',' '))
subprocess.run(['scp','-q']+K+[G+':/tmp/ve/all_%s.bin'%name,'.'],check=True)
