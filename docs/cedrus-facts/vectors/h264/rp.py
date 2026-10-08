import os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
import sys; sys.path.insert(0, HERE)
from hlab import *
from join import *
SC=ROOT+'/'
CACHE={}
def rows_for(trace,order):
    key=(trace,order)
    if key not in CACHE:
        CACHE[key]=(table(trace=SC+trace,order=order), runs(load(SC+trace)))
    return CACHE[key]
def replay(name, trace='multi.trace.txt', order='order.txt', dims=(176,144), over=None, tag='r', verbose=True):
    rows,R=rows_for(trace,order)
    idx=[i for i,r in enumerate(rows) if r['stream']==name]
    assert all(rows[i]['f']['nsl']=='1' for i in idx), 'multi-slice stream'
    w,h=dims
    runs_=[compact(R[i]) for i in idx]
    # slice files: from sl/ dir, the picture k of this stream
    sl=[H+'sl/%s_%d_0.bin'%(name,j) for j in range(len(idx))]
    text,dumps=replay_text(runs_,sl,w,h,over=over)
    open(H+'x_%s.lab'%tag,'w').write(text)
    files=' '.join(sl+[H+'x_%s.lab'%tag])
    r=sh('scp -q -i /root/.ssh/chimp_ed25519 %s %s:/tmp/ve/'%(files,G)); assert r.returncode==0, r.stderr
    r=sh('timeout 120 ssh -i /root/.ssh/chimp_ed25519 %s "cd /tmp/ve && ./ve_lab x_%s.lab 2>&1 | grep -E \'^228|TIMEOUT|rror\' | tr \'\\n\' \' \'"'%(G,tag))
    st=r.stdout
    disp=display_map(name)
    S=int(STRIDE) if STRIDE else (w+15)&~15; hmb=(h+15)//16
    res=[]
    for j,dn in enumerate(dumps):
        sh('scp -q -i /root/.ssh/chimp_ed25519 %s:/tmp/ve/%s %s%s'%(G,dn,H,dn))
        res.append(compare(H+dn,STREAMS+name+'.yuv',disp[j],w,h))
    ok=['+' if (a==b and c==d) else '-' for (a,b,c,d) in res]
    if verbose: print('%-10s %s  %s'%(name,''.join(ok),'TIMEOUT' if 'TIMEOUT' in st else ''))
    return res,st
if __name__=='__main__':
    for n in sys.argv[1:]: replay(n)
