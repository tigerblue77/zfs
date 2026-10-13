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

It passes at ashift 9 and 12.  Not covered: a persistent table, freeing or
reusing the old extent, freeing a redirected block, resilver and DTL
handling beyond the children's own DTL checks, and scale.
