# Spike: serving old-epoch RAID-Z blocks from re-encoded extents

Not for merge.  A throwaway experiment towards converting, in place, the
blocks of a RAID-Z vdev written before a parity change, including blocks
that snapshots still reference, without changing any block pointer.

`module/zfs/vdev_raidz.c` keeps a redirect table per raidz vdev (step 3
kept it in memory; step 5 makes it persistent and drops an entry when its
block is freed).  An entry maps a block, by
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
    crash_probe <dir> <ashift> guard <mos|zil|stale|checkpoint|wrongzb>

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

## Step 5, part B: freeing a moved block

Every decision to free a block (the DDT's refcount, the BRT's, gang trees,
deadlists, the free list and the deferred bpobj) is taken above
`metaslab_free_dva()` with the block pointer unchanged, and that function
already receives the physical birth.  There, a raidz DVA whose {offset,
birth} has an entry is converted: `vdev_raidz_redirect_remove()` takes the
entry out of memory and queues its key, and the caller frees
[O, O + old_reserved) and N instead of the old extent, each checkpointed by
its own birth (t1 and t2; no test takes a checkpoint).  A frozen pool frees
nothing and keeps the entry.  The vdev is dirtied for the syncing txg, and
`vdev_raidz_redirect_sync()`, called by `vdev_sync()` in every pass, removes
the queued keys from the table, so the frees and the removal commit under
the same uberblock.  `metaslab_check_free()` checks [O, O + old_reserved)
and N for a moved DVA, and the now=TRUE path asserts that no redirect
matches.

    crash_probe <dir> <ashift> free <plain|dedup|clone|gang|snap> <P1|P2>
    crash_probe <dir> <ashift> free plain P3 [nocheck]
    crash_probe <dir> <ashift> fcrash <TABLE_DIRTY|MS_SYNCED|PRE_UB|POST_UB> <P1|P2> [late]
    crash_probe <dir> <ashift> converge <P1|P2> [nolog]

`free` moves a plain object, the block of a dedup pair (refcount 2), the
block of a clone pair (one BRT reference), a gang header and its children,
and a block that only a snapshot holds, then frees one case.  The first
free of a pair must change nothing; after the free that converts, its txg
and TXG_DEFER_SIZE more, and again after an export and import, each freed
moved DVA's entry is gone from memory and from the ZAP, [O, O +
old_reserved) and N are in `ms_allocatable`, the bytes those extents held
dropped by exactly old_reserved + new_asize each, vs_alloc agrees with the
metaslab trees, every other object reads back, and zdb finds no leak.
`fcrash` frees the plain object with the crash points armed for the txg of
the free (`spike_crash_arm_free`): before POST_UB a fresh read-only import
finds the object, its entry and its extents, and reads it through the
redirect; at POST_UB all are gone; zdb is clean at every point.
`converge` frees 38 moved blocks of plain, dedup and gang objects in one
tx, with or without the log space map, and prints from
`spike_redirect_stats` how many were freed and removed in each sync pass.

At ashift 12 and 9 all of these pass under P1 and P2.  The plain, gang,
clone and snapshot frees convert in pass 1; the dedup block converts in
pass 2, after the DDT drops its last reference.  In `converge`, with the
log space map, 30 blocks convert in pass 1 and the 8 dedup blocks in pass 2
of the same txg, which takes 3 or 4 passes; without it, the 8 dedup frees
go to the deferred bpobj and convert in pass 1 of the next txg, after
`vdev_sync()`, and leave the table in its pass 2 (4 passes).  Each key
leaves the table in the txg of its free.  A free parked in the deferred
bpobj waits for a txg that changes something (`spa_sync()` skips the
deferred frees of a no-op txg); the probe dirties each txg it waits for.

On the previous step's code, freeing a moved block frees its old extent
and nothing else: under P1, N leaks (zdb: `leaked space` at N) and the
entry stays; under P2, the tail freed by the move is freed again, and the
debug build aborts in `metaslab_sync_done()` when the defer tree is vacated
into `ms_allocatable` (`adding segment ... overlapping`), or, after a crash
at POST_UB, zdb aborts loading the metaslab (`removing segment ... not
completely overlapped`).  `spike_redirect_late_remove` removes the key a
txg after the frees: F(POST_UB) then finds the entry of a freed block.
P3 keeps nothing, so a legitimate free of a moved block finds the whole old
extent free; with `nocheck` (`spike_redirect_check_enable = 0`) or on the
previous code, `metaslab_check_free()` aborts (`segment already in tree`).

## Step 5, fixes: resilvering moved blocks, stale block pointers

A resilver takes its window from the DTLs and prunes every block pointer
born before it, and `vdev_raidz_need_resilver()` tested the DTL at the old
birth and the columns of the old extent.  A block moved while a child was
offline was never visited: the resilver finished, cleared the child's DTL,
and left the child's column of N stale, so the block had lost a level of
parity without a trace.  A replace, whose window covers every txg, still
skipped a small block whose old columns missed the new child while N's
held it.  Now `dsl_scan_setup_sync()` lowers the window's min txg below
the old birth of every moved block whose new extent was written in the
window (`vdev_raidz_redirect_scan_min_txg()`), and
`vdev_raidz_need_resilver()` tests a moved block at N, with the DTL at its
new birth.  This traverses more of the pool than the window: a real
implementation would walk the table by new birth instead, which needs
enough of each block pointer in the table to verify the rebuild.  The
test-only `spike_resilver_redirects` holds one bit for each half (1 widens
the window, 2 tests N).

    crash_probe <dir> <ashift> resilver <P1|P2> <offline|offlinescrub|control|replace> [nofix|nowiden|nocols]

The setup is the free modes' plus small-Z, one sector of data.  `offline`
moves every block while child 2 is offline, brings it back and waits for
the resilver; `offlinescrub` then also scrubs; `control` keeps every child
online; `replace` replaces, with a new file, a child that holds a column of
small-Z's N and none of its old extent.  The target's column on that child
must not be all zero, and once two other columns of every N are inverted,
every object must read back, which parity 2 allows only if the resilver
rebuilt the child's column.

The move's only proof that a block was alive was that its old extent was
allocated, which an extent freed and reused by other blocks also is: the
move then freed the tail of a live block (P2) or leaked N for good (P1).
The move now resolves the block's bookmark in syncing context, as the error
scrub does (`dbuf_dnode_findbp()`), and requires the block pointer found
there to be the one it was given, or a gang block whose tree holds it, each
header read where it lies and checked against its own checksum; otherwise
it skips the block with ESTALE.  A bookmark must name where the block
lives: the snapshot, for a block that only a snapshot holds.  A bookmark
that names no block of an object is refused.  The test-only
`spike_redirect_move_live = 0` turns the check off.

    crash_probe <dir> <ashift> stalebp <P1|P2> [same] [nolive]

`stalebp` frees an object E, refills its old extent with live objects,
best fit, and moves E's saved block pointer.  Under `same` the refill holds
E's content, so that E's stale block pointer still verifies against what
lies at its offset: a check of the content alone would accept it.  `guard
wrongzb` moves live blocks, gang children among them, named by another
object's bookmark or at level -1.

On the previous step's libzpool, `offline` and `replace` fail under P1 and
P2 at ashift 12 and 9 while `control` and `offlinescrub` pass, `stalebp`
under P2 leaves a claim error on the block that took E's place and under P1
leaks N, and `wrongzb` moves everything.  With the fixes they all pass, and
each switch brings back its failure: `nofix` and `nocols` fail `offline`
and `replace`, `nowiden` fails `offline` only (a replace's window already
covers every txg), and `nolive` fails `stalebp`.

## Minor fixes: bookmarks, damaged entries, freed entries

The liveness check passed a bookmark at `DMU_SPILL_BLKID` to
`dbuf_dnode_findbp()`, which finds no block pointer for the spill block of
a dnode without one and fails `VERIFY3P(bp2, !=, NULL)` in the sync
thread.  The check now reads the spill block pointer itself, under
`dn_mtx`, from the dnode as the txg synced it, and refuses the bookmark
when the dnode has none.

    crash_probe <dir> <ashift> guard <spill|spillblk|xbook>

`spill` names plain-A by its object's absent spill block; `spillblk` gives
an object a spill block in phase 0 and moves it by its bookmark (it must
move and read back through its redirect after an import), and names
plain-A by that bookmark; `xbook` names plain-A by every other shape of
bookmark that reaches the check (bonus, negative or out-of-range blkid and
level, an object beyond the dnode count, freed or special, an objset that
is not a dataset) and then by its right one.  On the previous commit
`spill` aborts at ashift 12 and 9; `spillblk` and `xbook` pass there too,
so those shapes needed no change.

Not covered: a checkpoint-aware move or free (the flags are computed, never
exercised), a frozen pool, the table walk that should replace the widened
window and its cost on a large pool, a mirror beside the raidz vdev,
kernel builds, and scale.
