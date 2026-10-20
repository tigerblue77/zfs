# Check which erasure patterns an LRC built from ZFS RAID-Z generators can recover.
# Columns: k data, l local XOR parities (one per group of k/l), r global parities
# using the RAID-Z Q (2^i) and R (4^i) coefficients over GF(2^8), poly 0x11d.
import itertools, sys
EXP=[0]*512; LOG=[0]*256; x=1
for i in range(255):
    EXP[i]=x; LOG[x]=i; x<<=1
    if x&0x100: x^=0x11d
for i in range(255,512): EXP[i]=EXP[i-255]
def mul(a,b): return 0 if a==0 or b==0 else EXP[LOG[a]+LOG[b]]
def inv(a): return EXP[255-LOG[a]]
def rank(rows):
    m=[r[:] for r in rows]; rk=0; n=len(m[0]) if m else 0
    for c in range(n):
        p=next((i for i in range(rk,len(m)) if m[i][c]),None)
        if p is None: continue
        m[rk],m[p]=m[p],m[rk]; iv=inv(m[rk][c]); m[rk]=[mul(v,iv) for v in m[rk]]
        for i in range(len(m)):
            if i!=rk and m[i][c]:
                f=m[i][c]; m[i]=[a^mul(f,b) for a,b in zip(m[i],m[rk])]
        rk+=1
    return rk
def gen(k,l,r,gbase):
    rows=[[1 if i==j else 0 for j in range(k)] for i in range(k)]
    g=k//l
    for j in range(l): rows.append([1 if i//g==j else 0 for i in range(k)])
    for t in range(r):
        b=gbase[t]
        rows.append([EXP[(LOG[b]*(k-1-i))%255] for i in range(k)])
    return rows
def check(k,l,r,gbase=(2,4,8)):
    G=gen(k,l,r,gbase); n=len(G); out=[]
    for f in range(1,r+3):
        bad=tot=0
        for er in itertools.combinations(range(n),f):
            tot+=1
            if rank([G[i] for i in range(n) if i not in er])<k: bad+=1
        out.append((f,tot,bad))
    return out
for k,l,r in [(12,2,2),(8,2,2),(6,2,2),(16,4,2),(12,3,2),(12,2,1)]:
    res=check(k,l,r)
    print(f"k={k} l={l} r={r} width={k+l+r} overhead={(l+r)/(k+l+r):.1%} repair_reads={k//l}: "+
          "; ".join(f"{f} fail: {tot-bad}/{tot} ok ({100*(tot-bad)/tot:.1f}%)" for f,tot,bad in res))
