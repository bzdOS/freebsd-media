import sys, subprocess
K=['-i','/root/.ssh/chimp_ed25519']; G='root@192.0.2.82'
zz=[0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63]
intra_zz=[8,16,16,19,16,19,22,22,22,22,22,22,26,24,26,27,27,27,26,26,26,26,27,27,27,29,29,29,34,34,34,29,29,29,27,27,29,29,32,32,34,34,37,38,37,35,35,34,35,38,38,40,40,40,48,48,46,46,56,56,58,69,69,83]
raster=[0]*64
for i,p in enumerate(zz): raster[p]=intra_zz[i]
def build(name, intra, nonintra, ctrl=0x00130000, c114=0x80000008, hdr=0x1ffff340, tmpl_extra=None, end=0x1000, sel_intra=0x4000, sel_non=0):
    s=open('s_%s.bin'%name,'rb').read(); N=len(s)
    L=["reset","fill 0 0x800000 0","load s_%s.bin 0"%name,"wr 0x0ec 0x40","wr 0x0c4 0x400","wr 0x0c8 0x00200040","wr 0x000 0x%x"%ctrl]
    if intra is not None:
        for i,v in enumerate(intra): L.append("wr 0x180 0x%x"%(sel_intra|(i<<8)|v))
    if nonintra is not None:
        for i,v in enumerate(nonintra): L.append("wr 0x180 0x%x"%(sel_non|(i<<8)|v))
    L+=["wr 0x100 0x%x"%hdr,"wr 0x108 0x404","wr 0x10c 0x00400040",
     "wrpa 0x150 0x400000","wrpa 0x154 0x401000","wrpa 0x158 0x400000","wrpa 0x15c 0x401000","wrpa 0x148 0x400000","wrpa 0x14c 0x401000",
     "wr 0x12c 0","wr 0x130 %d"%(N*8),"wrvld 0x128 0 7","wrpa 0x134 0x%x"%end,"wr 0x110 0","wr 0x1c4 0","wr 0x1c8 0","wr 0x114 0x%x"%c114,
     "wr 0x118 0x8200000f","poll 0x11c 0x7 0x1 200","rd 0x11c","rd 0x1c4","rd 0x12c","dump 0x400000 0x10000 all_%s.bin"%name]
    return "\n".join(L)+"\n"
def run(name, lab, tag="x"):
    open('x_%s_%s.lab'%(name,tag),'w').write(lab)
    subprocess.run(['scp','-q']+K+['x_%s_%s.lab'%(name,tag),'s_%s.bin'%name,G+':/tmp/ve/'],check=True)
    r=subprocess.run(['ssh']+K+[G,'cd /tmp/ve && ./ve_lab x_%s_%s.lab 2>&1 | grep -E "^(11c|1c4|12c)"'%(name,tag)],capture_output=True,text=True,stdin=subprocess.DEVNULL)
    subprocess.run(['scp','-q']+K+[G+':/tmp/ve/all_%s.bin'%name,'.'],check=True)
    a=open('all_%s.bin'%name,'rb').read(); e=open(name+'.yuv','rb').read()
    ok=sum(1 for i in range(4096) if a[i]==e[i])
    okc=sum(1 for i in range(1024) if a[0x1000+2*i]==e[4096+i])+sum(1 for i in range(1024) if a[0x1000+2*i+1]==e[5120+i])
    print("%-22s"%tag, ' '.join(r.stdout.split()).replace('1c4 = 00000009 1c4 = 00000009','1c4=9'), "Y=%d/4096 C=%d/2048"%(ok,okc))
