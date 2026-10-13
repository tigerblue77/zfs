# Spike: serving old-epoch RAID-Z blocks from re-encoded extents

Not for merge.  A throwaway experiment towards converting, in place, the
blocks of a RAID-Z vdev written before a parity change, including blocks
that snapshots still reference, without changing any block pointer.

The commit adds an in-memory redirect table to `module/zfs/vdev_raidz.c`.
An entry maps a block, by its DVA offset and physical birth, to a new
extent re-encoded with the layout of a later txg.  `vdev_raidz_io_start()`
sends the I/O there (offset, layout and `io_txg`), and
`vdev_raidz_io_verify()` checks the map against its own birth.

`redirect_probe.c` builds a five-wide raidz1 pool of files, writes plain,
dedup and gang blocks, adds a parity-2 epoch, re-encodes some of the old
blocks into new extents, registers redirects and checks that:

* every block reads back bit-identical through its unchanged block pointer;
* two corrupted or missing columns are reconstructed and repaired in the new
  extent, three are not, and an unmoved parity-1 block fails with two;
* a scrub repairs a plain and a dedup block, and no healthy child reports a
  checksum error;
* a gang header and its children resolve and self-heal;
* the old extents are never written.

Build the tree with `./configure --with-config=user --enable-debug`, then:

    sh contrib/raidz-redirect-spike/build-tool.sh . contrib/raidz-redirect-spike/redirect_probe.c /tmp/redirect_probe
    cc -shared -fPIC -o /tmp/devzfs_shim.so contrib/raidz-redirect-spike/devzfs_shim.c -ldl
    mkdir /tmp/rdp && cd /tmp/rdp &&
        LD_PRELOAD=/tmp/devzfs_shim.so LD_LIBRARY_PATH=$OLDPWD/.libs \
        /tmp/redirect_probe /tmp/rdp 12

It passes at ashift 9 and 12.

## Step 4: what the move may do with the old extent

The move takes a policy for the old extent `[O, O + A)` of each block,
applied in its own sync task under `SCL_FREE` with `metaslab_free_concrete()`
and no checkpoint (asserted): P1 keeps it whole, P2 keeps its first sector
and frees the rest, P3 frees it whole.  The checks above always use P1.

    redirect_probe <dir> <ashift> brt <P1|P2|P3> [alone]

T4a, BRT reference theft.  A block X, cloned into a second object, is
moved; a new block Y is steered onto X's old extent (one allocator, best
fit, sized to fill what P2 frees) and freed.  Y must not start at O, X must
keep its one BRT reference, Y's extent must be allocatable again and both
objects must read back.  Under P3, Y starts at O and its free consumes X's
reference, so Y leaks; with `alone`, X being the vdev's only cloned block,
a debug build then aborts in `brt_vdev_destroy()`.  P1 and P2 pass.

    redirect_probe <dir> <ashift> scan <P1|P2|P3> [nofix]

T4b, sorted scrub.  The blocks of an eight-block run written in one txg
that have another run block right after them are moved, their new extents
claimed in the last metaslab, and blocks Y fill what the policy freed of
each old extent.  A sorted scrub must finish, issue every byte it
examined, repair a bad column of a moved and of a new block and find no
error.  The scan queues a moved block by its old extent, which overlaps Y
under P2 and P3; the extent tree's fill accounting then aborts the scrub
(`attempting to increase fill beyond max`).  This commit sends a redirected
block through the immediate path that gang blocks take
(`dsl_scan_enqueue()`, via `vdev_raidz_redirect_lookup()`); `nofix` clears
the test-only `spike_scan_redirect_unsorted` to show the failure again.  P1
passes either way.

Not covered: a persistent table, a checkpoint-aware move-time free, freeing
a redirected block, zdb, resilver and DTL handling beyond the children's own
DTL checks, and scale.
