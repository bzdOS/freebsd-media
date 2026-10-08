# SPDX-License-Identifier: BSD-2-Clause
import random
class BW:
    def __init__(s): s.b=[]
    def put(s,v,n):
        for i in range(n-1,-1,-1): s.b.append((v>>i)&1)
    def align(s):
        while len(s.b)%8: s.b.append(0)
    def bytes(s):
        s.align(); return bytes(int(''.join(map(str,s.b[i:i+8])),2) for i in range(0,len(s.b),8))
LUMA_SZ={0:'100',1:'00',2:'01',3:'101',4:'110',5:'1110',6:'11110',7:'111110',8:'1111110'}
CHR_SZ={0:'00',1:'01',2:'10',3:'110',4:'1110',5:'11110',6:'111110',7:'1111110',8:'11111110'}
def put_str(w,s):
    for ch in s: w.put(int(ch),1)
def dc(w,table,diff):
    size=abs(diff).bit_length()
    put_str(w,table[size])
    if size:
        w.put(diff if diff>0 else diff+(1<<size)-1,size)
    put_str(w,'10')       # end of block (table B.14, intra_vlc_format 0)
def picture(w_px,h_px,structure,tref,seed,ptype=1):
    """one I field/frame picture, DC only; returns (bytes, luma blocks map)"""
    out=b''
    bw=BW()
    # picture header
    bw.put(0x000001,24); bw.put(0x00,8); bw.put(tref,10); bw.put(ptype,3); bw.put(0xffff,16); bw.put(0,1)
    out+=bw.bytes()
    bw=BW(); bw.put(0x000001,24); bw.put(0xb5,8); bw.put(8,4)
    for _ in range(4): bw.put(15,4)
    bw.put(0,2); bw.put(structure,2); bw.put(1 if structure!=3 else 0,1)   # tff
    bw.put(1 if structure==3 else 0,1)          # frame_pred_frame_dct
    bw.put(0,1); bw.put(0,1); bw.put(0,1); bw.put(0,1)   # concealment,q_scale,intra_vlc,alt_scan
    bw.put(0,1); bw.put(1,1); bw.put(0,1); bw.put(0,1)   # repeat_first,chroma420,progressive_frame,composite
    out+=bw.bytes()
    fh=h_px//2 if structure!=3 else h_px
    rows=fh//16; cols=w_px//16
    rnd=random.Random(seed)
    blocks={}
    for r in range(rows):
        bw=BW(); bw.put(0x000001,24); bw.put(r+1,8); bw.put(8,5); bw.put(0,1)
        pred=[128,128,128]
        for c in range(cols):
            bw.put(1,1)      # macroblock_address_increment 1
            bw.put(1,1)      # macroblock_type: intra, no quantiser change
            for blk in range(6):
                comp=0 if blk<4 else blk-3
                v=rnd.randrange(24,232) if True else 0
                if blk<4: blocks[(r,c,blk)]=v
                diff=v-pred[comp]; pred[comp]=v
                dc(bw,LUMA_SZ if blk<4 else CHR_SZ,diff)
        out+=bw.bytes()
    return out
def stream(w_px,h_px,frames,field=True):
    bw=BW()
    bw.put(0x000001,24); bw.put(0xb3,8); bw.put(w_px,12); bw.put(h_px,12); bw.put(1,4); bw.put(3,4)
    bw.put(0x3ffff,18); bw.put(1,1); bw.put(112,10); bw.put(0,1); bw.put(0,1); bw.put(0,1)
    out=bw.bytes()
    bw=BW(); bw.put(0x000001,24); bw.put(0xb5,8); bw.put(1,4); bw.put(0x48,8); bw.put(0,1); bw.put(1,2); bw.put(0,2); bw.put(0,2)
    bw.put(0,12); bw.put(1,1); bw.put(0,8); bw.put(0,1); bw.put(0,2); bw.put(0,5)
    out+=bw.bytes()
    for f in range(frames):
        bw=BW(); bw.put(0x000001,24); bw.put(0xb8,8); bw.put(0,25); out+=bw.bytes()   # gop header: time code 0, closed gop
        if field:
            out+=picture(w_px,h_px,1,0,1000+f*2)
            out+=picture(w_px,h_px,2,0,1001+f*2)
        else:
            out+=picture(w_px,h_px,3,0,1000+f)
    out+=bytes([0,0,1,0xb7])
    return out
if __name__=='__main__':
    import sys
    open('gf.m2v','wb').write(stream(192,160,2,True))

def ppicture(w_px,h_px,structure,tref,seed,select_mode='rand'):
    """one P field/frame picture of 'MC, not coded' macroblocks with zero vectors; field_select per MB"""
    out=b''
    bw=BW()
    bw.put(0x000001,24); bw.put(0x00,8); bw.put(tref,10); bw.put(2,3); bw.put(0xffff,16)
    bw.put(0,1); bw.put(7,3); bw.put(0,1)        # full_pel_forward_vector, forward_f_code(7), extra bit
    out+=bw.bytes()
    bw=BW(); bw.put(0x000001,24); bw.put(0xb5,8); bw.put(8,4)
    bw.put(1,4); bw.put(1,4); bw.put(15,4); bw.put(15,4)     # f_codes: forward 1,1 ; backward unused
    bw.put(0,2); bw.put(structure,2); bw.put(1 if structure!=3 else 0,1)
    bw.put(1 if structure==3 else 0,1)
    bw.put(0,1); bw.put(0,1); bw.put(0,1); bw.put(0,1)
    bw.put(0,1); bw.put(1,1); bw.put(0,1); bw.put(0,1)
    out+=bw.bytes()
    fh=h_px//2 if structure!=3 else h_px
    rows=fh//16; cols=w_px//16
    rnd=random.Random(seed)
    sel={}
    for r in range(rows):
        bw=BW(); bw.put(0x000001,24); bw.put(r+1,8); bw.put(8,5); bw.put(0,1)
        for c in range(cols):
            bw.put(1,1)                  # address increment 1
            put_str(bw,'001')            # P: motion forward, not coded
            s=rnd.randrange(2) if select_mode=='rand' else int(select_mode)
            sel[(r,c)]=s
            if structure != 3:
                bw.put(1,2)              # field_motion_type: field-based prediction
                bw.put(s,1)              # motion_vertical_field_select[0][0]
            bw.put(1,1)                  # motion_code horizontal 0
            bw.put(1,1)                  # motion_code vertical 0
        out+=bw.bytes()
    return out,sel
def stream2(w_px,h_px,seed=5,select_mode='rand'):
    """frame A: I top + I bottom; frame B: P top + P bottom; frame C: P top + P bottom"""
    bw=BW()
    bw.put(0x000001,24); bw.put(0xb3,8); bw.put(w_px,12); bw.put(h_px,12); bw.put(1,4); bw.put(3,4)
    bw.put(0x3ffff,18); bw.put(1,1); bw.put(112,10); bw.put(0,1); bw.put(0,1); bw.put(0,1)
    out=bw.bytes()
    bw=BW(); bw.put(0x000001,24); bw.put(0xb5,8); bw.put(1,4); bw.put(0x48,8); bw.put(0,1); bw.put(1,2); bw.put(0,2); bw.put(0,2)
    bw.put(0,12); bw.put(1,1); bw.put(0,8); bw.put(0,1); bw.put(0,2); bw.put(0,5)
    out+=bw.bytes()
    bw=BW(); bw.put(0x000001,24); bw.put(0xb8,8); bw.put(0,25); out+=bw.bytes()
    out+=picture(w_px,h_px,1,0,seed)
    out+=picture(w_px,h_px,2,0,seed+1)
    for f in (1,2):
        out+=ppicture(w_px,h_px,1,f,seed+10*f,select_mode)[0]
        out+=ppicture(w_px,h_px,2,f,seed+10*f+1,select_mode)[0]
    out+=bytes([0,0,1,0xb7])
    return out
