# Spike: serving old-epoch RAID-Z blocks from re-encoded extents

Not for merge.  A throwaway experiment towards converting, in place, the
blocks of a RAID-Z vdev written before a parity change, including blocks
that snapshots still reference, without changing any block pointer.

`module/zfs/vdev_raidz.c` keeps a redirect table per raidz vdev (step 3
kept it in memory; step 5 makes it persistent).  An entry maps a block, by
its DVA offset and physical birth, to a new extent re-encoded with the
layout of a later txg.  `vdev_raidz_io_start()` sends the I/O there
(offset, layout and `io_txg`), and `vdev_raidz_io_verify()` checks the map
against its own birth.

`redirect_probe.c` builds a five-wide raidz1 pool of files, writes plain,
dedup and gang blocks, adds a parity-2 epoch, moves some of the old blocks
into new extents with the kernel's test-only move and checks that:

* every block reads back bit-identical through its unchanged block pointer;
* two corrupted or missing columns are reconstructed and repaired in the new
  extent, three are not, and an unmoved parity-1 block fails with two;
* a scrub repairs a plain and a dedup block, and no healthy child reports a
  checksum error;
* a gang header and its children resolve and self-heal;
* the old extents are never written.

Build the tree with `./configure --with-config=user --enable-debug`, then:

    sh contrib/raidz-redirect-spike/build-tool.sh . contrib/raidz-redirect-spike/redirect_probe.c /tmp/redirect_probe
    sh contrib/raidz-redirect-spike/build-tool.sh . contrib/raidz-redirect-spike/crash_probe.c /tmp/crash_probe
    cc -shared -fPIC -o /tmp/devzfs_shim.so contrib/raidz-redirect-spike/devzfs_shim.c -ldl
    mkdir /tmp/rdp && cd /tmp/rdp &&
        LD_PRELOAD=/tmp/devzfs_shim.so LD_LIBRARY_PATH=$OLDPWD/.libs \
        /tmp/redirect_probe /tmp/rdp 12

It passes at ashift 9 and 12.

## Step 4: what the move may do with the old extent

The move takes a policy for the old extent `[O, O + A)` of each block,
applied in its own sync task under `SCL_FREE` with `metaslab_free_concrete()`
and no checkpoint (since step 5 the move refuses one): P1 keeps it whole, P2
keeps its first sector and frees the rest, P3 frees it whole.  The checks
above always use P1.

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

## Step 5, part A: a persistent table, an atomic move, crash points, zdb

The table is now one ZAP per raidz vdev (`ZAP_FLAG_HASH64 |
ZAP_FLAG_UINT64_KEY`, as the BRT's), keyed by {old offset, physical birth},
holding {new offset, new birth, new asize, old_reserved}, and named in the
vdev's top-level ZAP under `org.openzfs:spike_raidz_redirects`.  In memory it
is an AVL tree in `vdev_raidz_t` under a rwlock, which the I/O path and the
scan routing take as readers; `vdev_raidz_load()` reads it right after the
parity-epoch table, before any non-MOS block is read, and
`vdev_raidz_fini()` frees it.

The move is a test-only kernel call, `vdev_raidz_redirect_move()`: the
caller reads the raw bytes, and one sync task of txg T refuses MOS blocks,
ZIL bookmarks and a pool with a checkpoint, skips a block whose old extent
is no longer wholly allocated (or is in `ms_freeing`, `ms_freed`,
`ms_defer` or `ms_checkpointing`), allocates N in T, writes it, adds the
entry (`zap_add_uint64` and the AVL insert) and frees what the policy does
not keep.  All of it lands in T.

zdb knows moved blocks: on a pool that is not writeable,
`metaslab_claim_dva()` claims [O, O + old_reserved) and N instead of the
old extent, a counter of what that adds goes into zdb's block sum, and the
table object counts as referenced in the MOS leak check.
`SPIKE_ZDB_IGNORE_REDIRECTS=claims|mos|all` in zdb's environment turns that
off, to show what zdb reports without it.

Crash points: `spike_crash_point` and `spike_crash_txg` (armed by the move
with T) make the process `_exit(99)` at TABLE_DIRTY (after the move's
table updates), MS_SYNCED (after `vdev_sync()` in `spa_sync`), PRE_UB (even
labels written, no uberblock) or POST_UB (after
`spa_sync_rewrite_vdev_config()`).

    crash_probe <dir> <ashift> move <TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [split]
    crash_probe <dir> <ashift> guard <mos|zil|stale|checkpoint>

    mkdir /tmp/crp && cd /tmp/crp && ulimit -c 0 &&
        ZDB=$OLDPWD/.libs/zdb LD_PRELOAD=/tmp/devzfs_shim.so \
        LD_LIBRARY_PATH=$OLDPWD/.libs /tmp/crash_probe /tmp/crp 12 move POST_UB P2

The parent never initializes libzpool; each phase is a forked child.
Phase 0 builds the pool (plain, dedup and gang objects and a control at
parity 1, then a parity-2 epoch).  Child A moves the plain, dedup and gang
blocks and the gang's children with the crash armed and must exit 99.
Child B imports read-only and checks the redirects loaded, every object
bit-exact, the redirect hits, and what is allocated, from the space maps
and the log space maps, under each metaslab's lock: the state before the
move at the first three points, the state after it at POST_UB.  The parent
then runs `$ZDB -bcc -d -Y -e -p <dir>` and requires no leaked space, a
matching block sum, no claim error and no leaked MOS object.  `split` sets
`spike_redirect_split_txg`, which adds the entries in a sync task of a
later txg (and arms the first three points in that txg): the state is then
neither.  On the previous step's libzpool (no persistent table, no crash
point), child A moves with the in-memory table and emulates POST_UB by
exiting once T has synced.

At ashift 12 and 9 the design passes every point under P1 and P2, and the
four guards.  The previous step's table fails at POST_UB: no redirect in a
fresh process, no hit, N leaked, and under P2 a claim error for each moved
block whose tail was freed.  The split variant fails at every point, and
zdb without its knowledge of moved blocks reports N leaked, those claim
errors, or the table object leaked.  `redirect_probe` gives the step 3 and
step 4 results unchanged.

Not covered: freeing a moved block (next part), a checkpoint-aware move,
resilver and DTL handling beyond the children's own DTL checks, kernel
builds, and scale.
