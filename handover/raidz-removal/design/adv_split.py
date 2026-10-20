#!/usr/bin/env python3
"""Independent re-derivation of: can a block cut at linear sector k (inside its psize span)
be read back if each piece is read from a non-adjacent raidz destination as a *standalone*
raidz block (what vdev_indirect_io_start does with bp == NULL at vdev_indirect.c:1369)?
Assumes pieces land at unrelated destination extents (no adjacency)."""
import importlib.util, os
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("adv_model", os.path.join(here, "adv_model.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def data_pos(n, w, p):
    """standalone block of n data sectors at b0=0: data index -> linear position (legacy map, bit20=0)"""
    cells = m.lin(m.legacy_cells(0, n, w, p, 0), w)
    return {role[1]: pos for pos, role in cells.items() if role[0] == 'D'}, cells

def check(w, p, s, k, even_only):
    # original block, b0 = 0 : linear pos -> role
    orig = m.lin(m.legacy_cells(0, s, w, p, 0), w)
    # content map for head extent [0,k) and tail extent [k, tot)
    # head standalone read of n1 = k data sectors
    n1, n2 = k, s - k
    dp1, _ = data_pos(n1, w, p)
    ok_head = True
    for d in range(n1):
        pos = dp1[d]
        if pos >= k:              # reads beyond the head extent -> foreign data
            ok_head = False; break
        if orig.get(pos) != ('D', d):
            ok_head = False; break
    dp2, _ = data_pos(n2, w, p)
    ok_tail = True
    for d in range(n2):
        pos = dp2[d]
        src = k + pos             # original linear position held at Y+pos
        if orig.get(src) != ('D', k + d):
            ok_tail = False; break
    return ok_head, ok_tail

tot_cases = both = head_only = tail_only = 0
for w in range(3, 9):
    for p in range(1, min(3, w - 1) + 1):
        for s in range(2, 3 * (w - p) + 1):
            tot = m.tot_sectors(s, w, p)
            for k in range(1, s):
                if k < p + 1:  # a piece shorter than p+1 sectors can't even be a valid standalone raidz alloc
                    pass
                h, t = check(w, p, s, k, False)
                tot_cases += 1
                both += (h and t); head_only += (h and not t); tail_only += (t and not h)
print("cases:", tot_cases, "head+tail both reproduce original:", both,
      "head only:", head_only, "tail only:", tail_only)
