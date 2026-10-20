# Pure-python model of vdev_raidz_map_alloc() column placement (module/zfs/vdev_raidz.c:640-728).
# Checks: (1) a block's sectors form a contiguous range in the linear space idx = row*dcols + child,
# (2) the logical data order (column-major gather, vdev_raidz_map_alloc_read) differs from linear order.
import sys

def map_alloc(b, s, dcols, p):
    f = b % dcols
    o = b // dcols                      # row (in sectors) on each child
    q = s // (dcols - p)
    r = s - q * (dcols - p)
    bc = 0 if r == 0 else r + p
    tot = s + p * (q + (0 if r == 0 else 1))
    if q == 0:
        acols = bc
        scols = min(dcols, -(-bc // (p + 1)) * (p + 1))
    else:
        acols = dcols
        scols = dcols
    cols = []
    for c in range(scols):
        col = f + c
        coff = o
        if col >= dcols:
            col -= dcols
            coff += 1
        if c >= acols:
            size = 0
        elif c < bc:
            size = q + 1
        else:
            size = q
        cols.append((c, col, coff, size))
    return cols, tot

def run(dcols, p, b, s):
    cols, tot = map_alloc(b, s, dcols, p)
    lin = []                               # (linear idx, role, label)
    dlabel = 0
    data_order = []
    # logical data order = columns c>=p in order, each rc_size sectors (map_alloc_read: off += rc_size)
    for (c, child, coff, size) in cols:
        for k in range(size):
            idx = (coff + k) * dcols + child
            if c < p:
                lin.append((idx, 'P%d' % c))
            else:
                lin.append((idx, 'D%d' % dlabel))
                dlabel += 1
    lin.sort()
    idxs = [x[0] for x in lin]
    contiguous = idxs == list(range(idxs[0], idxs[0] + len(idxs)))
    print("dcols=%d p=%d start=%d data_sectors=%d tot=%d  contiguous_linear=%s span=[%d,%d)"
          % (dcols, p, b, s, tot, contiguous, idxs[0], idxs[-1] + 1))
    print("   linear order:", " ".join(x[1] for x in lin))
    return contiguous

ok = True
ok &= run(5, 1, 0, 4)
ok &= run(5, 1, 0, 8)
ok &= run(5, 1, 3, 8)       # f != 0, wraps around the row
ok &= run(6, 2, 7, 10)
ok &= run(5, 1, 2, 3)       # q == 0 case
print("ALL CONTIGUOUS:", ok)
