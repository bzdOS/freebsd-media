import os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
import sys,subprocess
sys.path.insert(0, HERE)
from tr import *
D=ROOT+'/'
def table(trace=D+'multi.trace.txt', order='order.txt'):
    S='/opt/bzdos/linux-work/initramfs-h264/streams/'
    names=open(S+order).read().split()
    R=runs(load(trace))
    rows=[]; k=0
    for nm in names:
        out=subprocess.run([D+'h264/feat',S+nm+'.264'],capture_output=True,text=True).stdout.strip().split('\n')
        for line in out:
            f={}
            for kv in line.split():
                a,b=kv.split('=',1)
                f[a]=b
            r=compact(R[k]); k+=1
            regs={}; cmds=[]; sram=None; sram2=[]
            for x in r:
                if x[0]=='W':
                    regs[x[1]]=x[2]
                    if x[1]==0x224: cmds.append(x[2])
                elif x[0]=='SRAM':
                    if x[1]==0x400: sram=x[2]
                    else: sram2.append((x[1],x[2]))
            rows.append(dict(stream=nm,f=f,regs=regs,cmds=cmds,sram=sram,sram2=sram2,k=k-1))
    return rows
if __name__=='__main__':
    rows=table()
    print(len(rows))
