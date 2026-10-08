#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Print the fields an MPEG-2 elementary stream's headers give the engine:
width/height, picture_coding_type, f_codes, the picture coding extension and
the byte offset of the first slice start code. Plain ISO 13818-2 parsing."""
import sys
d=open(sys.argv[1],'rb').read()
i=0; out={}
def bits(b,pos,n):
    v=0
    for k in range(n):
        v=(v<<1)|((b[(pos+k)>>3]>>(7-((pos+k)&7)))&1)
    return v
while True:
    i=d.find(b'\x00\x00\x01',i)
    if i<0: break
    c=d[i+3]; p=(i+4)*8
    if c==0xb3:
        out['width']=bits(d,p,12); out['height']=bits(d,p+12,12)
    elif c==0x00:
        out['temporal_ref']=bits(d,p,10); out['pic_type']=bits(d,p+10,3)
        out['pic_off']=i
    elif c==0xb5 and bits(d,p,4)==8:
        q=p+4
        names=[('f00',4),('f01',4),('f10',4),('f11',4),('intra_dc_prec',2),('pic_struct',2),('top_first',1),('frame_pred',1),('conceal',1),('q_scale',1),('intra_vlc',1),('alt_scan',1),('repeat_first',1),('chroma420',1),('prog_frame',1),('composite',1)]
        for n,w in names:
            out[n]=bits(d,q,w); q+=w
    elif 1<=c<=0xaf and 'slice_off' not in out:
        out['slice_off']=i
    i+=3
for k,v in out.items(): print(k,v)
