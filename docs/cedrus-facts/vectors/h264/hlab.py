# SPDX-License-Identifier: BSD-2-Clause
# Replay a Linux H.264 decode run in the FreeBSD lab, with our own buffer layout.
import os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
import sys,subprocess,os,re
STRIDE=os.environ.get('H264_STRIDE')
STREAM_OFF=int(os.environ.get('H264_STREAM_OFF') or 0, 0)
H=HERE+'/'
sys.path.insert(0,H)
from tr import *
K=['-i','/root/.ssh/chimp_ed25519']; G='root@192.0.2.82'
def sh(c): return subprocess.run(c,shell=True,capture_output=True,text=True)
OFF=dict(strm=0x0, cur=0x20000, mvcol=0x80000, x250=0xa0000, x254=0xc0000)
def build(label, run, slfile, w, h, poc=(0,0), over=None, slot_override=None):
    """run = compact() ops of one Linux run; returns lab text. Address registers and the
    table are rebuilt for our layout; every other write is copied."""
    wmb,hmb=(w+15)//16,(h+15)//16
    S=int(STRIDE) if STRIDE else (w+15)&~15
    n=os.path.getsize(slfile)
    luma=OFF['cur']; chroma=luma+S*hmb*16
    mv_sz=wmb*hmb*16
    L=['reset','fill 0 0x100000 0','load %s %#x'%(os.path.basename(slfile),OFF['strm']),'sync']
    slot=[x[2] for x in run if x[0]=='W' and x[1]==0x24c][-1]
    if slot_override is not None: slot=slot_override
    for x in run:
        if x[0]=='W':
            off,val=x[1],x[2]
            if over and off in over: val=over[off]
            if off==0x250: L.append('wrpa 0x250 %#x'%OFF['x250'])
            elif off==0x254: L.append('wrpa 0x254 %#x'%OFF['x254'])
            elif off==0x230: L.append('wrvld 0x230 %#x 7'%OFF['strm'])
            elif off==0x23c: L.append('wrpa 0x23c %#x'%(OFF['strm']+n))
            elif off==0x238: L.append('wr 0x238 %d'%(n*8))
            elif off==0x24c: L.append('wr 0x24c %d'%slot)
            elif off==0x224 and val not in (7,8): L.append('wr 0x224 %#x'%val); L.append('poll 0x228 0x10000 0x10000 200')
            elif off==0x224 and val==8: L.append('wr 0x224 8'); L.append('poll 0x228 0x1 0x1 500'); L.append('rd 0x228')
            elif off==0x228: pass
            elif off==0x220: L.append('wr 0x220 0')
            else: L.append('wr %#x %#x'%(off,val))
        elif x[0]=='SRAM':
            base,words=x[1],x[2]
            if base==0x400:
                L.append('wr 0x2e0 0x400')
                for i in range(len(words)):
                    e,wd=divmod(i,8)
                    if e==slot and wd==0: L.append('wr 0x2e4 %d'%(poc[0]&0xffffffff))
                    elif e==slot and wd==1: L.append('wr 0x2e4 %d'%(poc[1]&0xffffffff))
                    elif e==slot and wd==3: L.append('wrpa 0x2e4 %#x'%luma)
                    elif e==slot and wd==4: L.append('wrpa 0x2e4 %#x'%chroma)
                    elif e==slot and wd==5: L.append('wrpa 0x2e4 %#x'%OFF['mvcol'])
                    elif e==slot and wd==6: L.append('wrpa 0x2e4 %#x'%(OFF['mvcol']+mv_sz))
                    else: L.append('wr 0x2e4 0')
            else:
                L.append('wr %#x %#x'%(0x2e0,base))
                for wv in words: L.append('wr 0x2e4 %#x'%wv)
    L.append('rd 0x228'); L.append('sync')
    L.append('dump %#x %#x out.bin'%(luma, S*hmb*16*3//2))
    return '\n'.join(L)+'\n'
def compare(outfile, yuv, frame, w, h):
    S=int(STRIDE) if STRIDE else (w+15)&~15; hmb=(h+15)//16
    o=open(outfile,'rb').read(); d=open(yuv,'rb').read(); fs=w*h*3//2; r=d[frame*fs:(frame+1)*fs]
    ny=sum(1 for y in range(h) for x in range(w) if o[y*S+x]==r[y*w+x])
    co=S*hmb*16; cw,ch=w//2,h//2
    nc=sum((o[co+y*S+2*x]==r[w*h+y*cw+x])+(o[co+y*S+2*x+1]==r[w*h+cw*ch+y*cw+x]) for y in range(ch) for x in range(cw))
    return ny,w*h,nc,2*cw*ch
def run_lab(text, slfile, tag='h'):
    open(H+'x_%s.lab'%tag,'w').write(text)
    r=sh('scp -q -i /root/.ssh/chimp_ed25519 %sx_%s.lab %s %s:/tmp/ve/'%(H,tag,slfile,G)); assert r.returncode==0, r.stderr
    r=sh('timeout 60 ssh -i /root/.ssh/chimp_ed25519 %s "cd /tmp/ve && ./ve_lab x_%s.lab 2>&1 | tail -12"'%(G,tag))
    sh('scp -q -i /root/.ssh/chimp_ed25519 %s:/tmp/ve/out.bin %sout_%s.bin'%(G,H,tag))
    return r.stdout

# ---------------------------------------------------------------- whole-stream replay
STREAMS='/opt/bzdos/linux-work/initramfs-h264/streams/'
def display_map(name):
    D_=HERE+'/'
    out=sh(D_+'dump3 '+STREAMS+name+'.264').stdout.split()
    pr=[(int(out[2*i]),int(out[2*i+1])) for i in range(len(out)//2)]
    disp={}; base=0; grp=[]
    def flush():
        nonlocal base
        order=sorted(range(len(grp)),key=lambda j:grp[j][1])
        for rank,j in enumerate(order): disp[grp[j][0]]=base+rank
        base+=len(grp)
    for k,(idr,poc) in enumerate(pr):
        if idr and grp: flush(); grp=[]
        grp.append((k,poc))
    flush()
    return disp
def replay_text(runs_, slfiles, w, h, over=None, maxpics=8, skip_sram_patch=False, regpatch=None):
    """runs_: list of compact() runs, one per picture (one slice each). Returns lab text and per-run dump names."""
    wmb,hmb=(w+15)//16,(h+15)//16
    S=int(STRIDE) if STRIDE else (w+15)&~15
    fsz=S*hmb*16*3//2
    mv_sz=wmb*hmb*16
    bufmap={}; mvmap={}
    nxt=[0x30000]; mvnxt=[0xe0000]
    def curbuf(L):
        if L not in bufmap:
            bufmap[L]=nxt[0]; nxt[0]+=(fsz+0xfff)&~0xfff
        return bufmap[L]
    def mvbuf(M):
        if M not in mvmap:
            mvmap[M]=mvnxt[0]; mvnxt[0]+=0x1000
        return mvmap[M]
    L_=['reset','fill 0 0x100000 0']
    off=STREAM_OFF; offs=[]
    for f in slfiles:
        L_.append('load %s %#x'%(os.path.basename(f),off)); offs.append(off); off+=((os.path.getsize(f)+0xfff)&~0xfff)
    assert off<=0x30000 or (STREAM_OFF>=0x100000 and off<=0x700000), off
    L_.append('sync')
    dumps=[]
    for ri,run in enumerate(runs_):
        n=os.path.getsize(slfiles[ri]); so=offs[ri]
        slot=[x[2] for x in run if x[0]=='W' and x[1]==0x24c][-1]
        cur_luma=None
        for x in run:
            if x[0]=='SRAM' and x[1]==0x400:
                cur_luma=x[2][8*slot+3]
        for x in run:
            if x[0]=='W':
                o,val=x[1],x[2]
                if over and o in over: val=over[o](val) if callable(over[o]) else over[o]
                if o==0x250: L_.append('wrpa 0x250 0xa0000')
                elif o==0x254: L_.append('wrpa 0x254 0xc0000')
                elif o==0x230: L_.append('wrvld 0x230 %#x 7'%so)
                elif o==0x23c: L_.append('wrpa 0x23c %#x'%(so+n))
                elif o==0x238: L_.append('wr 0x238 %d'%(n*8))
                elif o==0xc4: L_.append('wr 0xc4 %#x'%(S*hmb*16//4))
                elif o==0xc8: L_.append('wr 0xc8 %#x'%(((S//2)<<16)|S))
                elif o==0x224 and val not in (7,8): L_.append('wr 0x224 %#x'%val); L_.append('poll 0x228 0x10000 0x10000 200')
                elif o==0x224 and val==8: L_.append('wr 0x224 8'); L_.append('poll 0x228 0x1 0x1 500'); L_.append('sleep 20'); L_.append('rd 0x228')
                elif o==0x228: pass
                elif o==0x220: L_.append('wr 0x220 0')
                else: L_.append('wr %#x %#x'%(o,val))
            elif x[0]=='SRAM':
                base,words=x[1],x[2]
                L_.append('wr 0x2e0 %#x'%base)
                for i,wv in enumerate(words):
                    if base==0x400:
                        e,wd=divmod(i,8)
                        if wd in (3,4,5,6) and wv!=0:
                            Lb=words[8*e+3]; Mb=words[8*e+5]
                            if wd==3: L_.append('wrpa 0x2e4 %#x'%curbuf(Lb)); continue
                            if wd==4: L_.append('wrpa 0x2e4 %#x'%(curbuf(Lb)+S*hmb*16)); continue
                            if wd==5: L_.append('wrpa 0x2e4 %#x'%mvbuf(Mb)); continue
                            if wd==6: L_.append('wrpa 0x2e4 %#x'%(mvbuf(Mb)+mv_sz)); continue
                    L_.append('wr 0x2e4 %#x'%wv)
        L_.append('sync')
        L_.append('dump %#x %#x out_%d.bin'%(curbuf(cur_luma),fsz,ri)); dumps.append('out_%d.bin'%ri)
    return '\n'.join(L_)+'\n', dumps
def run_replay(name, k0, npics, w, h, over=None, tag='r'):
    T=ROOT+'/multi.trace.txt'
    return T
