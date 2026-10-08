import re,subprocess,sys
ORDER=sys.argv[1]; CON=sys.argv[2]
S='/opt/bzdos/linux-work/initramfs-h264/streams/'
names=open(S+ORDER).read().split()
con=open(CON).read()
# decode PIC lines grouped by STREAM
txt=re.sub(r'\[ *[0-9.]*\] ','',con).replace('\n',' ')
parts=re.split(r'STREAM\s+0x',txt)[1:]
dims={'s64':(64,64),'s192':(192,160),'s320':(320,240),'hd640':(640,368),'hd720':(1280,720)}
res={}
for i,p in enumerate(parts[:len(names)]):
    pics=re.findall(r'PIC\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)',p)
    res[names[i]]=[(int(a,16),int(b,16),int(c,16)) for a,b,c in pics]
def fnv(b):
    h=2166136261
    for x in b: h=((h^x)*16777619)&0xffffffff
    return h
bad=0
for n in names:
    w,h=dims.get(n,(176,144))
    d=open(S+n+'.yuv','rb').read(); fs=w*h*3//2
    out=subprocess.run([HERE+'/dump3',S+n+'.264'],capture_output=True,text=True).stdout.split()
    # dump3 prints "idr poc" per picture
    pr=[(int(out[2*i]),int(out[2*i+1])) for i in range(len(out)//2)]
    # display index
    disp=[]; base=0; grp=[]
    def flush():
        global base
        order=sorted(range(len(grp)),key=lambda j:grp[j][1])
        for rank,j in enumerate(order): disp.append((grp[j][0],base+rank))
        base+=len(grp)
    idx=[]
    for k,(idr,poc) in enumerate(pr):
        if idr and grp: flush(); grp=[]
        grp.append((k,poc))
    flush()
    dmap=dict(disp)
    ok=[]
    for (k,poc,st),(hv) in zip([(i,r[1],r[2]) for i,r in enumerate(res[n])],[r[0] for r in res[n]]):
        pass
    line=[]
    # res tuples are (npic, poc, type) hash was the 4th group; re-extract hashes
    hs=re.findall(r'PIC\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x([0-9a-f]+)',parts[names.index(n)])
    for k,hx in enumerate(hs):
        exp=fnv(d[dmap[k]*fs:dmap[k]*fs+w*h])
        ok=(int(hx,16)==exp); line.append('+' if ok else '-')
        if not ok: bad+=1
    print('%-10s %s'%(n,''.join(line)))
print('mismatches',bad)
