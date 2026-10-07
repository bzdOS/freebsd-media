import sys
# Each argument is <file>[:<variant>].  The variant says which control value the
# rig forces for this run of the stream, relative to what the parser read:
#   0 = as parsed (control run), 1 = cabac_init_idc 2, 2 = explicit B weights.
names=sys.argv[1:]
def split(a):
    if ':' in a:
        p,v=a.rsplit(':',1)
        return p,int(v)
    return a,0
with open('h264s.h','w') as f:
    for i,a in enumerate(names):
        n,_=split(a)
        d=open(n,'rb').read()
        f.write('static const unsigned char h264_s%d[] = {%s};\n'%(i,','.join(str(b) for b in d)))
    f.write('static const struct { const unsigned char *data; unsigned long len; int variant; } h264_streams[] = {\n')
    for i,a in enumerate(names):
        n,v=split(a)
        d=open(n,'rb').read()
        f.write('  { h264_s%d, %d, %d },\n'%(i,len(d),v))
    f.write('};\n#define H264_NSTREAMS %d\n'%len(names))
