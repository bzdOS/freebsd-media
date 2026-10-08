# SPDX-License-Identifier: BSD-2-Clause
import sys
def load(fn='multi.trace.txt'):
    ents=[]
    for l in open(fn):
        x=l.split()
        ents.append((int(x[0]),x[2],x[3],int(x[4],16),int(x[5],16)))
    return ents
def runs(ents):
    idx=[i for i,e in enumerate(ents) if e[2]=='VE' and e[1]=='W32' and e[3]==0xec]
    idx.append(len(ents))
    return [ents[idx[k]:idx[k+1]] for k in range(len(idx)-1)]
def compact(run):
    """list of ('W',off,val) / ('R',off,val) / ('SRAM',base,[words])"""
    out=[]; i=0
    while i<len(run):
        e=run[i]
        if e[2]=='VE' and e[3]==0x2e0 and e[1]=='W32':
            base=e[4]; i+=1; words=[]
            while i<len(run) and run[i][3]==0x2e4 and run[i][1]=='W32':
                words.append(run[i][4]); i+=1
            out.append(('SRAM',base,words))
        else:
            out.append((e[1][0],e[3],e[4],e[2])); i+=1
    return out
if __name__=='__main__':
    ents=load()
    R=runs(ents)
    print('runs',len(R))
