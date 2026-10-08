import os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
import sys; sys.path.insert(0, HERE)
from join import *
D_=ROOT+'/'
sets=[(D_+'multi.trace.txt','order.txt'),(D_+'multi2.trace.txt','order2.txt'),(D_+'multi3.trace.txt','order3.txt')]
def s6(v): return v&63
def s4(v): return v&15
bad={}
n=0
for tr,order in sets:
    rows=table(trace=tr,order=order)
    for r in rows:
        f=r['f']; g=r['regs']; n+=1
        wm,hm=int(f['wmb']),int(f['hmb']); st=int(f['stype']); pf=int(f['ppsflags'],16)
        l0=int(f['nref'].split('/')[0]); l1=int(f['nref'].split('/')[1])
        if st==2: l0=l1=0
        if st==0: l1=0
        exp={}
        exp[0x200]=0x0d0000|((wm-1)<<8)|(hm-1)
        exp[0x204]=((pf&1)<<15)|(l0<<10)|(l1<<5)|(((pf>>2)&1)<<4)|(int(f['wbi'])<<2)|(((pf>>4)&1)<<1)|((pf>>6)&1)
        e208=((0)<<24)  # first_mb
        fm=int(f['firstmb']); e208=((fm%wm)<<24)|((fm//wm)<<16)|((1 if int(f['nalref'])!=0 else 0)<<12)|(st<<8)|(int(f['direct'])<<2)
        exp[0x208]=e208
        cq=lambda s: int(f['cqp'].split('/')[s])
        exp[0x20c]=0x1000|(l0<<24)|(l1<<16)|(int(f['dbf'])<<8)|((int(f['alpha'])&15)<<4)|(int(f['beta'])&15)
        qp=int(f['initqp'])+int(f['qpd'])
        exp[0x21c]=((0 if (pf&0x80) else 1)<<24)|(s6(cq(1))<<16)|(s6(cq(0))<<8)|qp
        S=wm*16
        exp[0xc8]=((S//2)<<16)|S
        exp[0xc4]=S*hm*16//4
        exp[0x238]=int(f['len'])*8
        for o,v in exp.items():
            got=g[o]
            if o==0x208: got&=~0x20
            if got!=v: bad.setdefault((hex(o)),[]).append((r['stream'],r['k'],hex(got),hex(v)))
        # skip commands
        tot=0
        for c in r['cmds']:
            if c in (7,8): continue
            tot+=(c>>8)
        if tot!=int(f['hdr']): bad.setdefault('skip',[]).append((r['stream'],r['k'],tot,f['hdr']))
        # lists
        # weights denominators
print('runs',n)
for k,v in bad.items(): print(k,len(v),v[:4])
