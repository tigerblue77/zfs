#!/usr/bin/env python3
"""Second pass: realistic expansion width pairs, VERIFY-removed overlap, free mis-target.
Imports the model from adv_model.py in the same directory (run with: python3 -I -c ...)."""
import importlib.util, sys, os
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("adv_model", os.path.join(here, "adv_model.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def first_diff_s(Ls, Ld, p, wphys, smax=200):
    for s in range(1, smax):
        a = m.lin(m.expanded_cells(0, s, wphys, Ls, p), wphys)
        b = m.lin(m.expanded_cells(0, s, wphys, Ld, p), wphys)
        if a != b:
            return s
    return None

print("first data size (sectors) whose layout differs between two logical widths")
for (Ls, Ld, wp, p) in ((4,5,5,1),(5,6,6,1),(4,6,6,1),(6,7,7,2),(8,9,9,2)):
    s = first_diff_s(Ls, Ld, p, wp)
    print("  p=%d L=%d vs L=%d: first differing s = %d sectors (= %d KiB at ashift 12, %d B at ashift 9)"
          % (p, Ls, Ld, s, s*4, s*512))
print()

# VERIFY3U(DVA_GET_ASIZE(&dst), ==, size) at vdev_removal.c:1239 : what a raidz dst would return
for (w, p) in ((5,1),(6,2),(8,2),(10,3)):
    size_sectors = (16 << 20) >> 12   # default zfs_remove_max_segment = 16 MiB, ashift 12
    A = m.asize_sectors(size_sectors, w, p)
    print("raidz%d w=%d: 16 MiB segment allocated as psize -> asize %d sectors = %.2f MiB (overlaps next entry by %.2f MiB if VERIFY is relaxed)"
          % (p, w, A, A*4/1024, (A-size_sectors)*4/1024))
print()

# cut-phase census for a 128 KiB block, ashift 12
for (w, p) in ((5,1),(6,2),(8,2)):
    s = 32
    tot = m.tot_sectors(s, w, p); A = m.asize_sectors(s, w, p)
    split = s - 1             # cuts k=1..s-1 : psize-based read sees a split
    tail = tot - s            # cuts k=s..tot-1: non-split by psize, footprint overruns
    skip = A - tot            # cuts inside the skip sectors: harmless
    print("raidz%d w=%d 128K block: A=%d tot=%d | cut phases: split(k<s)=%d overrun(s<=k<tot)=%d skip-only=%d boundary=1 of %d phases"
          % (p, w, A, tot, split, tail, skip, A))
    harmful = split + tail
    print("     harmful fraction of cut phases = %.1f%%; for an 8 TiB pool with 512Ki cuts of 16 MiB: ~%d harmful cuts"
          % (100.0*harmful/A, int(524288*harmful/A)))
print()

# D3-with-short-mapping: free of (O, A) when the mapping holds only psize
S = 32*4096; A = 40*4096
print("free of BP asize %d B when mapping holds only psize %d B: release build leftover=%d B is applied at dst_offset - %d B (unsigned wrap)"
      % (A, S, A-S, A-S))
# memory for per-block entries vs per-16MiB segments (24 B/entry)
for tib, avg in ((8,128*1024),(8,16*1024),(8,8*1024)):
    n = tib * (1<<40) // avg
    seg = tib * (1<<40) // (16<<20)
    print("%d TiB allocated, avg block %d KiB: per-block entries %d (%.2f GiB RAM @24 B) vs per-16MiB entries %d (%.1f MiB)"
          % (tib, avg//1024, n, n*24/(1<<30), seg, seg*24/(1<<20)))
