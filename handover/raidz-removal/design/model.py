# Python transcription of the ROLE assignment in vdev_raidz_map_alloc_expanded()
# (module/zfs/vdev_raidz.c lines ~810-960). INFERRED model, not the real code.
import itertools, sys

def layout(s, L, p, swap=False):
    """Return dict linear_sector_rel -> role, plus tot.  role = ('P',i) or ('D',idx)."""
    q = s // (L - p)
    r = s - q * (L - p)
    bc = 0 if r == 0 else r + p
    tot = s + p * (q + (0 if r == 0 else 1))
    rows = -(-tot // L)
    cols = min(tot, L)
    out = {}
    for row in range(rows):
        for c in range(cols):
            if c < p:
                role = ('P', c)
            elif row == rows - 1 and bc != 0 and c >= bc:
                continue
            else:
                dc = c - p
                if c < bc or r == 0:
                    off = dc * rows + row
                else:
                    off = r * rows + (dc - r) * (rows - 1) + row
                role = ('D', off)
            out[row * L + c] = role
    if swap and p == 1 and cols > 1:
        for row in range(rows):
            a, b = row * L + 0, row * L + 1
            if a in out and b in out:
                out[a], out[b] = out[b], out[a]
    assert len(out) == tot, (len(out), tot)
    return out, tot, rows

def asize_sectors(s, L, p):
    # vdev_raidz_psize_to_asize_width
    a = s + p * ((s + L - p - 1) // (L - p))
    return -(-a // (p + 1)) * (p + 1)

# 1. every data index 0..s-1 appears exactly once
for L in range(3, 9):
    for p in (1, 2, 3):
        if L <= p: continue
        for s in range(1, 60):
            o, tot, rows = layout(s, L, p)
            idx = sorted(v[1] for v in o.values() if v[0] == 'D')
            assert idx == list(range(s)), (L, p, s)
print("data-index permutation is a bijection: OK")

# 2. child-run rotation: S linear b -> D linear b+delta ; child j run -> single D child run
w = 6
for delta in range(0, 40):
    seen = {}
    for b in range(0, 200):
        j, r = b % w, b // w
        j2, r2 = (b + delta) % w, (b + delta) // w
        assert j2 == (j + delta) % w
        assert r2 == r + (j + delta) // w
print("child-run rotation property: OK")

# 3. split analysis. For each block shape (s,L,p) and each linear split k, can head/tail
#    be read as STANDALONE raidz blocks by the same L and stitch back by contiguous slices?
def standalone_s_for_tot(tot_target, L, p):
    res = [s for s in range(1, 4 * tot_target + 4) if layout(s, L, p)[1] == tot_target]
    return res

bad = good = unrep = 0
goodlist = []
for L in (4, 5, 6, 8):
    for p in (1, 2):
        for s in range(1, 3 * (L - p) + 1):
            o, tot, rows = layout(s, L, p)
            for k in range(1, tot):
                hs = standalone_s_for_tot(k, L, p)
                ts = standalone_s_for_tot(tot - k, L, p)
                if not hs or not ts:
                    unrep += 1
                    continue
                ok = False
                for s1 in hs:
                    for s2 in ts:
                        if s1 + s2 != s: continue
                        h, _, _ = layout(s1, L, p)
                        t, _, _ = layout(s2, L, p)
                        # head must reproduce original roles at linear positions < k
                        # (data idx identical), tail data idx shifted by s1, tail linear positions
                        # are relative to k
                        okh = all(o.get(x) == h[x] for x in h) and len(h) == k
                        # tail positions: original linear sectors k.. k+len; standalone tail starts at its own row 0
                        # original sectors k..tot-1 are not necessarily contiguous (gaps in last row) so compare by order
                        osorted = [o[x] for x in sorted(o) if x >= k]
                        tsorted = [(v[0], v[1] + (s1 if v[0] == 'D' else 0)) for x, v in sorted(t.items())]
                        okt = osorted == tsorted
                        if okh and okt:
                            ok = True
                if ok:
                    good += 1; goodlist.append((L, p, s, k, tot))
                else:
                    bad += 1
print("splits not representable as standalone blocks:", unrep)
print("representable but roles differ:", bad)
print("representable and identical roles:", good)
print(goodlist[:20])

# 4. weaker test: ignore parity sectors entirely, only require data sectors to land at the same
#    relative position and with the same data index after stitching by contiguous slices.
weak = []
for L in (4, 5, 6, 8):
    for p in (1, 2):
        for s in range(1, 3 * (L - p) + 1):
            o, tot, rows = layout(s, L, p)
            for k in range(1, tot):
                hs = standalone_s_for_tot(k, L, p)
                ts = standalone_s_for_tot(tot - k, L, p)
                for s1 in hs:
                    for s2 in ts:
                        if s1 + s2 != s: continue
                        h, _, _ = layout(s1, L, p)
                        t, _, _ = layout(s2, L, p)
                        od = [(x, v[1]) for x, v in sorted(o.items()) if v[0] == 'D']
                        hd = [(x, v[1]) for x, v in sorted(h.items()) if v[0] == 'D']
                        td = [(x + k, v[1] + s1) for x, v in sorted(t.items()) if v[0] == 'D']
                        # tail linear positions relative: compare sequence of data idx only
                        if [i for _, i in od] == [i for _, i in hd] + [i for _, i in td]:
                            weak.append((L, p, s, k, tot))
print("data-index sequence identical (parity ignored):", len(weak), weak[:10])
