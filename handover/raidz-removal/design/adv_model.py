#!/usr/bin/env python3
"""
Adversarial model of OpenZFS RAID-Z block placement (lens: on-disk safety).

Transcribed independently from module/zfs/vdev_raidz.c (master 4542a317e2fa):
  - vdev_raidz_map_alloc          (legacy map, lines 640-773)
  - vdev_raidz_map_alloc_expanded (lines 791-1000)
  - vdev_raidz_psize_to_asize_width (lines 2376-2388)
Units are sectors (1 << ashift); a "block" is (offset b0 in sectors, s data
sectors).  Nothing here touches /home/user/zfs or any pool.
"""
import itertools
import random
import sys


def asize_sectors(s, L, p):
    """vdev_raidz_psize_to_asize_width, in sectors (psize already in sectors)."""
    a = s
    a += p * ((a + L - p - 1) // (L - p))
    # roundup(a, p+1)
    return -(-a // (p + 1)) * (p + 1)


def tot_sectors(s, L, p):
    q = s // (L - p)
    r = s - q * (L - p)
    return s + p * (q + (1 if r else 0))


def legacy_cells(b0, s, w, p, bit20=0):
    """vdev_raidz_map_alloc: dict (child,row) -> role, role = ('P',i)|('D',idx).
    bit20: value of (io_offset & 1<<20) != 0 (only matters when p == 1)."""
    f = b0 % w
    o = b0 // w
    q = s // (w - p)
    r = s - q * (w - p)
    bc = 0 if r == 0 else r + p
    if q == 0:
        acols = bc
        scols = min(w, -(-bc // (p + 1)) * (p + 1))
    else:
        acols = w
        scols = w
    cols = []
    for c in range(scols):
        col = f + c
        coff = o
        if col >= w:
            col -= w
            coff += 1
        if c >= acols:
            size = 0
        elif c < bc:
            size = q + 1
        else:
            size = q
        cols.append([col, coff, size])
    if p == 1 and bit20:
        cols[0][0], cols[1][0] = cols[1][0], cols[0][0]
        cols[0][1], cols[1][1] = cols[1][1], cols[0][1]
    cells = {}
    dataoff = 0
    for c, (child, row0, size) in enumerate(cols):
        for j in range(size):
            if c < p:
                cells[(child, row0 + j)] = ('P', c, j)
            else:
                cells[(child, row0 + j)] = ('D', dataoff + j)
        if c >= p:
            dataoff += size
    return cells


def expanded_cells(b0, s, w_phys, L, p, bit20=0):
    """vdev_raidz_map_alloc_expanded without any reflow in flight."""
    q = s // (L - p)
    r = s - q * (L - p)
    bc = 0 if r == 0 else r + p
    tot = s + p * (q + (1 if r else 0))
    rows = -(-tot // L)
    ncols = min(tot, L)
    cells = {}
    rr_cols = []
    for row in range(rows):
        b = b0 + row * L
        child = b % w_phys
        coff = b // w_phys
        rowcols = []
        for c in range(ncols):
            if child >= w_phys:
                child -= w_phys
                coff += 1
            dc = c - p
            if c < p:
                role = ('P', c, row)
            elif row == rows - 1 and bc != 0 and c >= bc:
                role = None
            else:
                if c < bc or r == 0:
                    off = dc * rows + row
                else:
                    off = r * rows + (dc - r) * (rows - 1) + row
                role = ('D', off)
            rowcols.append([child, coff, role])
            child += 1
        if p == 1 and ncols > 1 and bit20:
            rowcols[0][0], rowcols[1][0] = rowcols[1][0], rowcols[0][0]
            rowcols[0][1], rowcols[1][1] = rowcols[1][1], rowcols[0][1]
        for child_, coff_, role in rowcols:
            if role is not None:
                cells[(child_, coff_)] = role
    return cells


def lin(cells, w):
    """(child,row)->role  ==>  linear sector index -> role"""
    return {row * w + child: role for (child, row), role in cells.items()}


def check_model_equivalence(trials=20000, seed=1):
    """legacy map == expanded map when L == w (compare roles ignoring parity row tag)."""
    random.seed(seed)
    bad = 0
    for _ in range(trials):
        w = random.randint(3, 9)
        p = random.randint(1, min(3, w - 1))
        s = random.randint(1, 5 * (w - p))
        b0 = random.randint(0, 200)
        bit20 = random.randint(0, 1) if p == 1 else 0
        a = legacy_cells(b0, s, w, p, bit20)
        b = expanded_cells(b0, s, w, w, p, bit20)
        norm = lambda d: {k: (v[0], v[1] if v[0] == 'D' else 0) for k, v in d.items()}
        # parity roles differ in naming only; compare data placement and parity cell sets
        da = {k: v for k, v in a.items() if v[0] == 'D'}
        db = {k: v for k, v in b.items() if v[0] == 'D'}
        pa = {k for k, v in a.items() if v[0] == 'P'}
        pb = {k for k, v in b.items() if v[0] == 'P'}
        if da != db or pa != pb:
            bad += 1
    return bad


def check_translation_invariance(trials=20000, seed=2):
    """Role of the k-th linear sector of a block is independent of b0 (p>1 or
    same bit 20); a delta with bit 20 flipped changes roles when p == 1."""
    random.seed(seed)
    bad_nonp1_or_same20 = 0
    flipped_p1_diff = 0
    flipped_p1_total = 0
    for _ in range(trials):
        w = random.randint(3, 9)
        p = random.randint(1, min(3, w - 1))
        s = random.randint(1, 5 * (w - p))
        b0 = random.randint(0, 300)
        delta = random.randint(0, 5000)
        bit20_a = random.randint(0, 1) if p == 1 else 0
        a = lin(legacy_cells(b0, s, w, p, bit20_a), w)
        for bit20_b in ((0, 1) if p == 1 else (0,)):
            b = lin(legacy_cells(b0 + delta, s, w, p, bit20_b), w)
            ra = {k - b0: v for k, v in a.items()}
            rb = {k - (b0 + delta): v for k, v in b.items()}
            if bit20_b == bit20_a:
                if ra != rb:
                    bad_nonp1_or_same20 += 1
            else:
                flipped_p1_total += 1
                if ra != rb:
                    flipped_p1_diff += 1
    return bad_nonp1_or_same20, flipped_p1_diff, flipped_p1_total


def width_mismatch_demo(s, p, L_src, L_dst, w_phys):
    """Block written when logical width was L_src, read by a vdev that decodes
    it with L_dst.  Returns (asize_src, asize_dst, tot_src, tot_dst,
    number of linear positions whose role differs)."""
    a = lin(expanded_cells(0, s, w_phys, L_src, p), w_phys)
    b = lin(expanded_cells(0, s, w_phys, L_dst, p), w_phys)
    keys = set(a) | set(b)
    diff = sum(1 for k in keys if a.get(k) != b.get(k))
    return (asize_sectors(s, L_src, p), asize_sectors(s, L_dst, p),
            tot_sectors(s, L_src, p), tot_sectors(s, L_dst, p), diff)


def single_row_blocks_ignore_width(w_phys, p, s_max):
    """Which data sizes s are decoded identically for every logical width in
    [p+1, w_phys]? (small-block blindness of naive tests)"""
    out = []
    for s in range(1, s_max + 1):
        layouts = set()
        for L in range(p + 1, w_phys + 1):
            if s + p > L and s > 0 and (tot_sectors(s, L, p) > L):
                pass
            cells = lin(expanded_cells(0, s, w_phys, L, p), w_phys)
            layouts.add(tuple(sorted(cells.items())))
        out.append((s, len(layouts)))
    return out


def overrun_demo(w, p, s, cut):
    """Non-split decision uses psize (s sectors) but raidz reads the whole
    footprint (tot sectors).  cut = sectors held by the first mapping entry.
    Returns list of foreign (position, role) the destination would read and
    whether they can be 'reconstructed' (<= p bad cols in every row)."""
    cells = lin(expanded_cells(0, s, w, w, p), w)
    tot = tot_sectors(s, w, p)
    assert cut >= s, "cut inside psize would be a split read"
    foreign = [k for k in range(cut, tot)]
    perrow = {}
    for k in foreign:
        perrow.setdefault(k // w, 0)
        perrow[k // w] += 1
    recoverable = all(v <= p for v in perrow.values())
    return tot, asize_sectors(s, w, p), foreign, [cells.get(k) for k in foreign], recoverable


if __name__ == "__main__":
    print("model equivalence (legacy == expanded at L==w), mismatches:",
          check_model_equivalence())
    print("translation invariance (violations when bit20 equal / p>1, "
          "role changes when p==1 and bit 20 flipped, trials):",
          check_translation_invariance())
    print()
    # CE-B: 128 KiB block, ashift 12 => s = 32 sectors, raidz1
    for (Ls, Ld, wp) in ((4, 5, 5), (5, 6, 6), (4, 6, 6)):
        r = width_mismatch_demo(32, 1, Ls, Ld, wp)
        print("raidz1 s=32 sectors: written at L=%d, decoded at L=%d: "
              "asize %d vs %d, tot %d vs %d, roles that differ: %d"
              % (Ls, Ld, r[0], r[1], r[2], r[3], r[4]))
    for (Ls, Ld, wp) in ((6, 7, 7),):
        r = width_mismatch_demo(32, 2, Ls, Ld, wp)
        print("raidz2 s=32 sectors: written at L=%d, decoded at L=%d: "
              "asize %d vs %d, tot %d vs %d, roles that differ: %d"
              % (Ls, Ld, r[0], r[1], r[2], r[3], r[4]))
    print()
    # which small sizes are width-blind (identical layout for all widths)?
    for p, wp in ((1, 5), (2, 6)):
        res = single_row_blocks_ignore_width(wp, p, 12)
        print("p=%d w=%d: s -> #distinct layouts over L in [%d,%d]: %s"
              % (p, wp, p + 1, wp, res))
    print()
    # CE-A: overrun on a raidz destination
    for (w, p, s, cut) in ((5, 1, 32, 39), (5, 1, 32, 36), (5, 1, 4, 4), (6, 2, 32, 40),
                           (6, 2, 32, 38)):
        tot, A, foreign, roles, rec = overrun_demo(w, p, s, cut)
        print("w=%d p=%d s=%d cut=%d: tot=%d asize=%d foreign sectors=%s roles=%s "
              "checksum-reconstructible=%s" % (w, p, s, cut, tot, A, foreign, roles, rec))
