#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
import sys, re, subprocess
bs = sys.argv[1]; maxp = int(sys.argv[2]) if len(sys.argv) > 2 else 60
r = subprocess.run(['ffmpeg','-v','debug','-debug','pict+mmco','-i',bs,'-frames:v',str(maxp),'-f','null','-'],capture_output=True,text=True)
lines = (r.stderr).split('\n')
# ffmpeg: [h264 @ ...] short term list: / "<i> fn:N poc:P 0xptr" / long term list: / "<i> fn:N poc:P" / slice:...
states=[]; cur_s=[]; cur_l=[]; mode=None; pics=[]
for ln in lines:
    ln = re.sub(r'^\[h264 @ [0-9a-fx]+\] ','',ln)
    if ln.startswith('short term list:'): mode='s'; cur_s=[]; continue
    if ln.startswith('long term list:'): mode='l'; cur_l=[]; continue
    m = re.match(r'(\d+) fn:(\d+) poc:(-?\d+)', ln)
    if m and mode:
        (cur_s if mode=='s' else cur_l).append((int(m.group(2)),int(m.group(3)),int(m.group(1)))); continue
    m = re.match(r'slice:(\d+) ([FTB]) mb:(\d+) .*? frame:(\d+) poc:(-?\d+)/(-?\d+)', ln)
    if m:
        mode=None
        key=(m.group(2), int(m.group(4)), m.group(5), m.group(6))
        if not pics or pics[-1][4]!=key:
            pics.append((m.group(2), int(m.group(4)), list(cur_s), list(cur_l), key))
    # the lists printed after marking become the state for the next picture
# avformat_find_stream_info decodes the start first: the real decode begins at the second occurrence of the first picture
k0=pics[0][4] if pics else None
for j in range(1,len(pics)):
    if pics[j][4]==k0: pics=pics[j:]; break
print(len(pics),'ffmpeg pictures')
pics=pics[:maxp]
mine = subprocess.run(['/opt/bzdos/linux-work/h264front/dpbcmp',bs,str(maxp)],capture_output=True,text=True).stdout.strip().split('\n')
import difflib
mk=[]
for b in mine:
    m = re.match(r'([FTB]) (\d+) (-?\d+)/(-?\d+) :(.*)', b)
    mk.append((m.group(1), int(m.group(2))) if m else ('E',0))
fk=[(a[0],a[1]) for a in pics]
sm=difflib.SequenceMatcher(None,fk,mk,autojunk=False)
bad=0; matched=0
for blk in sm.get_matching_blocks():
    for t in range(blk.size):
        a=pics[blk.a+t]; b=mine[blk.b+t]; i=blk.b+t
        m = re.match(r'([FTB]) (\d+) (-?\d+)/(-?\d+) :(.*)', b)
        mine_s = sorted(int(x.group(1)) for x in re.finditer(r'S(\d+)/', m.group(5)))
        mine_l = sorted(int(x.group(1)) for x in re.finditer(r'L(\d+)/', m.group(5)))
        ff_s = sorted(set(fn for fn,_,_ in a[2])); ff_l = sorted(set(idx for _,_,idx in a[3]))
        matched+=1
        flag=''
        if mine_s!=ff_s: flag='SHORT'
        elif mine_l!=ff_l: flag='LONG'
        if flag:
            bad+=1
            if bad<=4: print(i, flag, 'ffmpeg', a[0], a[1], ff_s, ff_l, '| mine', m.group(1), m.group(2), mine_s, mine_l)
print('aligned',matched,'of',len(pics),'mismatches',bad)
