import subprocess, struct, os, sys
K=['-i','/root/.ssh/chimp_ed25519']; G='root@192.0.2.82'
def sh(c, **kw): return subprocess.run(c, shell=True, capture_output=True, text=True, **kw)
def mkvec(name, w, h, enc_opts='', src='testsrc2', frames=1, vf=''):
    """encode `frames` frames (GOP 1 => all I unless opts say otherwise); return m2v path"""
    vff = ('-vf "%s"' % vf) if vf else ''
    c=('ffmpeg -v error -y -f lavfi -i "%s%ssize=%dx%d:rate=25" %s -frames:v %d -c:v mpeg2video -q:v 3 -g 1 -bf 0 -pix_fmt yuv420p %s %s.m2v'
       % (src, '=' if '=' not in src else ':', w, h, vff, frames, enc_opts, name))
    r=sh(c); assert r.returncode==0, r.stderr
    r=sh('ffmpeg -v error -y -i %s.m2v -f rawvideo -pix_fmt yuv420p %s.yuv' % (name,name)); assert r.returncode==0, r.stderr
def bits(b,pos,n):
    v=0
    for k in range(n): v=(v<<1)|((b[(pos+k)>>3]>>(7-((pos+k)&7)))&1)
    return v
def parse(name):
    """headers of the first picture + slice data (first slice start code to the next non-slice start code)"""
    d=open(name+'.m2v','rb').read(); i=0; o={}
    qi=None; qn=None
    while True:
        i=d.find(b'\x00\x00\x01',i)
        if i<0: break
        c=d[i+3]; p=(i+4)*8
        if c==0xb3:
            o['w']=bits(d,p,12); o['h']=bits(d,p+12,12); q=p+12+12+4+4+18+1+10+1
            # sequence header: load_intra flag, matrix, load_non_intra, matrix
            if bits(d,q,1): o['iq']=[bits(d,q+1+8*k,8) for k in range(64)]; q+=1+512
            else: q+=1
            if bits(d,q,1): o['nq']=[bits(d,q+1+8*k,8) for k in range(64)]
        elif c==0x00 and 'ptype' not in o:
            o['ptype']=bits(d,p+10,3); o['poff']=i
        elif c==0xb5 and bits(d,p,4)==8 and 'f00' not in o:
            q=p+4
            for n,wd in [('f00',4),('f01',4),('f10',4),('f11',4),('idc',2),('pstruct',2),('tff',1),('fpd',1),('conceal',1),('qs',1),('ivlc',1),('alt',1),('rff',1),('c420',1),('prog',1),('comp',1)]:
                o[n]=bits(d,q,wd); q+=wd
        elif c==0xb5 and bits(d,p,4)==3:
            pass
        elif 1<=c<=0xaf and 'soff' not in o:
            o['soff']=i
        elif ('soff' in o) and (c==0x00 or c==0xb3 or c==0xb7 or c==0xb8):
            o['send']=i; break
        i+=3
    o.setdefault('send', len(d))
    o['slice']=d[o['soff']:o['send']]
    return o

DEF_INTRA=[8,16,16,19,16,19,22,22,22,22,22,22,26,24,26,27,27,27,26,26,26,26,27,27,27,29,29,29,34,34,34,29,29,29,27,27,29,29,32,32,34,34,37,38,37,35,35,34,35,38,38,40,40,40,48,48,46,46,56,56,58,69,69,83]
LUMA=0x80000
def hdr_i(o, ptype=None):
    pt=o['ptype'] if ptype is None else ptype
    return (pt<<28)|(o['f00']<<24)|(o['f01']<<20)|(o['f10']<<16)|(o['f11']<<12)|(o['idc']<<10)|(o['pstruct']<<8)|(o['tff']<<7)|(o['fpd']<<6)|(o['conceal']<<5)|(o['qs']<<4)|(o['ivlc']<<3)|(o['alt']<<2)
def lab(o, name, hdr, c114=0x800000b8, chroma=None, w_mb=None, h_mb=None, wpx=None, hpx=None, c10c=None, c108=None, ctrl=0x130000, c8=0x00200040, c4=0x400, extra_pre='', luma=LUMA, nbits=None, vbv=None, length=None, loadoff=0, vldoff=0, bitoff=0):
    w=o['w']; h=o['h']
    wm=(w+15)//16 if w_mb is None else w_mb; hm=(h+15)//16 if h_mb is None else h_mb
    N=len(o['slice']) if length is None else length
    if chroma is None: chroma=luma+((wm*16*hm*16+0xfff)&~0xfff)
    iq=o.get('iq',DEF_INTRA); nq=o.get('nq',[16]*64)
    L=["reset","fill 0 0x100000 0","load s_%s.bin 0x%x"%(name,loadoff),"sync","batch","wr 0x0ec 0x40","wr 0x0c4 0x%x"%c4,"wr 0x0c8 0x%08x"%c8,"wr 0x000 0x%x"%ctrl]
    for i,v in enumerate(iq): L.append("wr 0x180 0x%x"%(0x4000|(i<<8)|v))
    for i,v in enumerate(nq): L.append("wr 0x180 0x%x"%((i<<8)|v))
    L.append(extra_pre)
    L+=["wr 0x100 0x%x"%hdr,
        "wr 0x108 0x%x"%((wm<<8|hm) if c108 is None else c108),
        "wr 0x10c 0x%08x"%(((hpx if hpx else hm*16)<<16|(wpx if wpx else wm*16)) if c10c is None else c10c)]
    for r,a in ((0x150,luma),(0x154,chroma),(0x158,luma),(0x15c,chroma),(0x148,luma),(0x14c,chroma)): L.append("wrpa 0x%x 0x%x"%(r,a))
    L+=["wr 0x12c %d"%bitoff,"wr 0x130 %d"%(N*8 if nbits is None else nbits),"wrvld 0x128 0x%x 7"%vldoff,"wrpa 0x134 0x%x"%(loadoff+N if vbv is None else vbv),
        "wr 0x110 0","wr 0x1c4 0","wr 0x1c8 0","wr 0x114 0x%x"%c114,"wr 0x118 0x8200000f","go",
        "poll 0x11c 0x7 0x1 300","rd 0x11c","rd 0x1c4","rd 0x12c","sync","dump 0x80000 0x80000 out_%s.bin"%name]
    open('x_%s.lab'%name,'w').write("\n".join(L)+"\n")
    open('s_%s.bin'%name,'wb').write(o['slice'])
def run(name):
    r=sh('scp -q -i /root/.ssh/chimp_ed25519 x_%s.lab s_%s.bin %s:/tmp/ve/'%(name,name,G)); assert r.returncode==0, r.stderr
    r=sh('timeout 30 ssh -i /root/.ssh/chimp_ed25519 %s "cd /tmp/ve && ./ve_lab x_%s.lab 2>&1 | grep -E \'^(11c|1c4|12c|poll)\'"'%(G,name))
    sh('scp -q -i /root/.ssh/chimp_ed25519 %s:/tmp/ve/out_%s.bin .'%(G,name))
    st={}
    for l in r.stdout.splitlines():
        p=l.split()
        if p[0] in('11c','1c4','12c'): st[p[0]]=int(p[2],16)
    return st
def compare(name, w, h, stride=None, chroma_off=None, frame=0, tol=1):
    out=open('out_%s.bin'%name,'rb').read(); ref=open(name+'.yuv','rb').read()
    fs=w*h*3//2; ref=ref[frame*fs:(frame+1)*fs]
    best=None
    for S in ([stride] if stride else sorted({w,(w+15)&~15,(w+31)&~31,(w+63)&~63})):
        n=0
        for y in range(h):
            a=out[y*S:y*S+w]; b=ref[y*w:y*w+w]
            n+=sum(1 for i in range(w) if abs(a[i]-b[i])<=tol)
        if best is None or n>best[1]: best=(S,n)
    S=best[0]
    return best, S

def chroma_score(name, w, h, chroma_addr_off, cstride, tol=1):
    out=open('out_%s.bin'%name,'rb').read(); ref=open(name+'.yuv','rb').read()
    u=ref[w*h:w*h+(w//2)*(h//2)]; v=ref[w*h+(w//2)*(h//2):w*h+2*(w//2)*(h//2)]
    n=0
    for y in range(h//2):
        for x in range(w//2):
            if abs(out[chroma_addr_off+y*cstride+2*x]-u[y*(w//2)+x])<=tol: n+=1
            if abs(out[chroma_addr_off+y*cstride+2*x+1]-v[y*(w//2)+x])<=tol: n+=1
    return n, (w//2)*(h//2)*2

def full(name, w=None, h=None, **kw):
    """encode already done; parse, decode on the engine with stride = aligned width, return (status, hdr, Ymatch, Cmatch)"""
    o=parse(name); w=o['w']; h=o['h']
    wa=(w+15)&~15; ha=(h+15)&~15
    S=wa
    hd=kw.pop('hdr',None)
    if hd is None: hd=hdr_i(o)
    lab(o,name,hd,c8=((S//2)<<16)|S,chroma=LUMA+((S*ha+0xfff)&~0xfff),**kw); st=run(name)
    out=open('out_%s.bin'%name,'rb').read(); ref=open(name+'.yuv','rb').read()
    nY=sum(1 for y in range(h) for x in range(w) if abs(out[y*S+x]-ref[y*w+x])<=2)
    co=(S*ha+0xfff)&~0xfff
    cw,ch=w//2,h//2
    u=ref[w*h:w*h+cw*ch]; v=ref[w*h+cw*ch:w*h+2*cw*ch]
    nC=sum((abs(out[co+y*S+2*x]-u[y*cw+x])<=2)+(abs(out[co+y*S+2*x+1]-v[y*cw+x])<=2) for y in range(ch) for x in range(cw))
    return st, hd, nY, w*h, nC, 2*cw*ch, o

def parse_all(name):
    """every picture in coded order: dict(ptype, tref, f00.., idc, pstruct, flags..., slice bytes)"""
    d=open(name+'.m2v','rb').read()
    hdr=parse(name)
    pics=[]; i=0; cur=None
    while True:
        i=d.find(b'\x00\x00\x01',i)
        if i<0: break
        c=d[i+3]; p=(i+4)*8
        if c==0x00:
            if cur is not None: cur['send']=i; pics.append(cur); cur=None
            cur={'tref':bits(d,p,10),'ptype':bits(d,p+10,3),'poff':i}
        elif c in (0xb3,0xb8,0xb7):
            if cur is not None and 'soff' in cur: cur['send']=i; pics.append(cur); cur=None
        elif c==0xb5 and cur is not None and bits(d,p,4)==8:
            q=p+4
            for n,wd in [('f00',4),('f01',4),('f10',4),('f11',4),('idc',2),('pstruct',2),('tff',1),('fpd',1),('conceal',1),('qs',1),('ivlc',1),('alt',1),('rff',1),('c420',1),('prog',1),('comp',1)]:
                cur[n]=bits(d,q,wd); q+=wd
        elif 1<=c<=0xaf and cur is not None and 'soff' not in cur:
            cur['soff']=i
        i+=3
    if cur is not None and 'soff' in cur: cur['send']=len(d); pics.append(cur)
    for pc in pics:
        pc['slice']=d[pc['soff']:pc['send']]
        pc['iq']=hdr.get('iq'); pc['nq']=hdr.get('nq')
    return hdr, pics

FB0=0x20000; FBSZ=0xC000; SGAP=0x2000
def fbaddr(k): return FB0+k*FBSZ
def hdr_pic(p):
    return (p['ptype']<<28)|(p['f00']<<24)|(p['f01']<<20)|(p['f10']<<16)|(p['f11']<<12)|(p['idc']<<10)|(p['pstruct']<<8)|(p['tff']<<7)|(p['fpd']<<6)|(p['conceal']<<5)|(p['qs']<<4)|(p['ivlc']<<3)|(p['alt']<<2)
def lab_multi(name, hdr, pics, plan, w=176, h=144, c114=0x80000080, trig=0x8200000f, extra_for=None, nfb=None, hmb=None, rec_off=None):
    """plan: list of (pic_index, fwd_fb, back_fb, rec_fb). extra_for(k,p)-> list of extra register lines"""
    wm=(w+15)//16; hm=(h+15)//16; S=(w+31)&~31
    chroma_off=S*hm*16
    if hmb is None: hmb=hm
    L=["reset","fill 0 0x100000 0"]
    for i,p in enumerate(pics):
        open('s_%s_%d.bin'%(name,i),'wb').write(p['slice'])
        L.append("load s_%s_%d.bin 0x%x"%(name,i,i*SGAP))
    L.append("sync")
    iq=hdr.get('iq',DEF_INTRA); nq=hdr.get('nq',[16]*64)
    for k,(pi,fw,bk,rec) in enumerate(plan):
        p=pics[pi]; N=len(p['slice']); so=pi*SGAP
        L+=["batch","wr 0x0ec 0x40","wr 0x0c4 0","wr 0x0c8 0x%08x"%(((S//2)<<16)|S),"wr 0x000 0x130000"]
        for i,v in enumerate(iq): L.append("wr 0x180 0x%x"%(0x4000|(i<<8)|v))
        for i,v in enumerate(nq): L.append("wr 0x180 0x%x"%((i<<8)|v))
        if extra_for: L+=extra_for(k,p)
        L+=["wr 0x100 0x%x"%hdr_pic(p),"wr 0x108 0x%x"%((wm<<8)|(hmb(k,p) if callable(hmb) else hmb)),"wr 0x10c 0x%08x"%((wm*16<<16)|((hmb(k,p) if callable(hmb) else hmb)*16))]
        for r,a in ((0x150,fbaddr(fw)),(0x154,fbaddr(fw)+chroma_off),(0x158,fbaddr(bk)),(0x15c,fbaddr(bk)+chroma_off),(0x148,fbaddr(rec)),(0x14c,fbaddr(rec)+chroma_off)): L.append("wrpa 0x%x 0x%x"%(r,a))
        L+=["wr 0x12c 0","wr 0x130 %d"%(N*8),"wrvld 0x128 0x%x 7"%so,"wrpa 0x134 0x%x"%(so+N),"wr 0x110 0","wr 0x1c4 0","wr 0x1c8 0","wr 0x114 0x%x"%c114,"wr 0x118 0x%x"%trig,"go",
            "poll 0x11c 0x7 0x1 300","rd 0x11c","rd 0x1c4","wr 0x114 0x80000080","wr 0x11c 7"]
    L.append("sync")
    L.append("dump 0x%x 0x%x fb_%s.bin"%(FB0,FBSZ*(nfb if nfb else 9),name))
    open('x_%s.lab'%name,'w').write("\n".join(L)+"\n")
def run_multi(name):
    files=' '.join(['x_%s.lab'%name]+['s_%s_%d.bin'%(name,i) for i in range(32) if os.path.exists('s_%s_%d.bin'%(name,i))])
    r=sh('scp -q -i /root/.ssh/chimp_ed25519 %s %s:/tmp/ve/'%(files,G)); assert r.returncode==0, r.stderr
    r=sh('timeout 60 ssh -i /root/.ssh/chimp_ed25519 %s "cd /tmp/ve && ./ve_lab x_%s.lab 2>&1 | grep -E \'^(11c|1c4|poll)\'"'%(G,name))
    sh('scp -q -i /root/.ssh/chimp_ed25519 %s:/tmp/ve/fb_%s.bin .'%(G,name))
    return r.stdout
def score_fb(name, k, refframe, w=176, h=144, yuvname=None):
    out=open('fb_%s.bin'%name,'rb').read(); ref=open((yuvname or name)+'.yuv','rb').read()
    S=(w+31)&~31; base=k*FBSZ; fs=w*h*3//2; r=ref[refframe*fs:(refframe+1)*fs]
    ha=(h+15)&~15; co=S*ha
    nY=sum(1 for y in range(h) for x in range(w) if abs(out[base+y*S+x]-r[y*w+x])<=2)
    cw,ch=w//2,h//2
    nC=sum((abs(out[base+co+y*S+2*x]-r[w*h+y*cw+x])<=2)+(abs(out[base+co+y*S+2*x+1]-r[w*h+cw*ch+y*cw+x])<=2) for y in range(ch) for x in range(cw))
    return nY,w*h,nC,2*cw*ch

def mbmap(name, k, refframe, w=176, h=144, tol=2, yuvname=None):
    out=open('fb_%s.bin'%name,'rb').read(); ref=open((yuvname or name)+'.yuv','rb').read()
    S=(w+31)&~31; base=k*FBSZ; fs=w*h*3//2; r=ref[refframe*fs:(refframe+1)*fs]
    rows=[]
    for my in range((h+15)//16):
        row=''
        for mx in range((w+15)//16):
            bad=sum(1 for y in range(16) for x in range(16) if abs(out[base+(my*16+y)*S+mx*16+x]-r[(my*16+y)*w+mx*16+x])>tol)
            row+='.' if bad==0 else ('x' if bad>128 else 'o')
        rows.append(row)
    return rows

def make_plan(pics):
    """frame-buffer assignment in coded order for I/P/B (B never a reference); returns plan and frame index per picture"""
    plan=[]; prev=None; last=None; nxt=0
    for i,p in enumerate(pics):
        rec=nxt; nxt+=1
        if p['ptype']==1: fw=bk=rec
        elif p['ptype']==2: fw=bk=last
        else: fw=prev; bk=last
        plan.append((i,fw,bk,rec))
        if p['ptype']!=3: prev,last=last,rec
    return plan

def enc_pb(name, frames, extra, src='testsrc2', w=176, h=144, rate=25, vf=''):
    vff=('-vf "%s"'%vf) if vf else ''
    c=('ffmpeg -v error -y -f lavfi -i "%s%ssize=%dx%d:rate=%s" %s -frames:v %d -c:v mpeg2video -q:v 5 %s -pix_fmt yuv420p %s.m2v'%(src,"=" if "=" not in src else ":",w,h,rate,vff,frames,extra,name))
    r=sh(c); assert r.returncode==0, r.stderr
    r=sh('ffmpeg -v error -y -i %s.m2v -f rawvideo -pix_fmt yuv420p %s.yuv'%(name,name)); assert r.returncode==0

def check_seq(name, w, h, c10c_mode=0):
    hdr,pics=parse_all(name)
    plan=make_plan(pics)
    wm=(w+15)//16; hm=(h+15)//16
    lab_multi(name,hdr,pics,plan,w=w,h=h,nfb=len(pics))
    ok=run_multi(name).count('c001')
    res=[]
    for k,p in enumerate(pics):
        nY,tY,nC,tC=score_fb(name,k,p['tref'],w=w,h=h)
        res.append(('IPB'[p['ptype']-1] if p['ptype']<4 else '?')+('+' if (nY==tY and nC==tC) else '-%d'%(tY-nY)))
    return ok,len(pics),' '.join(res)
