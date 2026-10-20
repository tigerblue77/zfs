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
birth and the columns of the old extent (step 6 below replaces the first
half of this fix).  A block moved while a child was
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

The table load checked only that N was inside the vdev, non-empty and
born after the block.  Each entry must now be one the move could have
written: its key two uint64s and its value five (25 since step 6); the
old extent [O, O + old asize) and N aligned extents of one metaslab, N
clear of the old extent; old_reserved non-zero, aligned and no larger
than the old asize; the new birth after the birth, no later than the
pool's last txg and in a later parity epoch.  The value gains the old
asize for that: nothing else in the entry bounds old_reserved.  The move
refuses a block of the current epoch, and P3, which keeps nothing, unless
the test-only `spike_redirect_keep_none` is set, as the probes do for P3
(zdb, through `SPIKE_ZDB_KEEP_NONE`).

A damaged entry fails a read-write import with EROFS: ignored, its block
would be freed whole at an old extent part of which may now be another
block's, and read and repaired there; used, it would send the same I/O and
frees elsewhere.  Failing every import, or dropping the table, would make
every moved block unreadable.  A read-only import, which allocates, frees
and repairs nothing, loads the sound entries and leaves the damaged ones
out, with a warning naming each: their blocks are read at their old
extents, under the checksum.  zdb, which imports read-only, prints the
warnings and reports what follows (a claim error on the block when P2 freed
its tail, N leaked).  What the load cannot see is whether an entry fits the
block that uses it: the free and zdb's claim compare the entry's old asize
with the DVA's, the free then leaks what the entry holds with a warning and
the claim fails; a read whose N is too small for the block fails with EIO
instead of a VERIFY.

    crash_probe <dir> <ashift> corrupt <kind> [P1|P2]

After the free modes' setup and move (P2 unless P1 is given), plain-A's
entry is rewritten on disk: `misalign`, `nasize`, `nout`, `nspan`, `nzero`
(N misaligned, of misaligned size, outside the vdev, across two
metaslabs, empty), `noverlap` (N at O), `reserved0`, `rmisalign`,
`overreserve` (old_reserved zero, misaligned, old asize + 64K), `birth`,
`epoch`, `future` (new birth equal to the birth, before the parity-2
epoch, 1000 txgs ahead), `koffset`, `kout`, `ksize` (key offset
misaligned, beyond the vdev, a third key integer), `vsize` (three values);
`oasize` (old_reserved and old asize both 64K too large) and `nsmall` (N
one sector) pass the load.  A read-write import must refuse each of the
others with EROFS, a read-only one must load the sound entries, read every
other object and never return wrong bytes for plain-A, and zdb must run to
its end and name the entry.  For `oasize` and `nsmall`, the read-write
import reads plain-A (through N, or EIO) and frees it, which must leak and
not free twice.  On the previous commit, with the probe built against its
headers, at ashift 12 and 9 under P2: `nout`, `nzero`, `birth` and `vsize`
fail every import of the pool, read-only ones and zdb's included
(EINVAL); every other kind is accepted by a read-write import, and
freeing plain-A then aborts, except for `epoch`, `future` and `reserved0`
(and `nsmall` aborts on the read); zdb aborts for `misalign`, `nasize`,
`rmisalign`, `koffset`, `kout`, `ksize` and `nsmall`.  `ksize` also showed
that the old load took a key it could not look up for the end of the
table: it loaded 2 of 8 entries at ashift 12, 2 of 7 at ashift 9.

Freeing a moved block took its entry out of memory at once, while
[O, O + old_reserved) and N stayed in the defer trees for TXG_DEFER_SIZE
txgs.  I/O issued with the freed block's pointer in that window (a stale
holder's read, a scan I/O already queued) missed the table and went to O,
whose tail P2 freed at the move and another block may hold: a checksum
error, or, when only one column of O differs, a reconstruction that
succeeds and a self-heal write into the other block.  The free now leaves
the entry in the tree as a tombstone, which I/O, resilver and scan routing
still follow to N, and which the free path, the claims, the counts and the
scan window ignore.  `vdev_sync_dispatch()` drops it in the txg whose sync
moves the free's extents out of the defer trees, before it does (in the
txg of the free itself when that txg defers no free, as
`metaslab_sync_done()` decides).  [O, O + old_reserved) is freed with N,
so no other block can be born at O while the tombstone stands, and none
born later has its key.  Nothing of a tombstone is persisted.

    crash_probe <dir> <ashift> tomb <read|repair>

After the free modes' setup and a P2 move of all but small-Z, plain-A is
freed and its saved block pointer read with `zio_read()` as soon as the
free's txg has synced.  `read` first moves small-Z with its N claimed at
the start of plain-A's freed tail, a live block inside O; `repair`
overwrites one sector of a data column of plain-A's freed tail on disk, as
a block written there would.  The read must return plain-A's bytes through
one redirect hit and leave every column of O as it was; the tombstone must
stand while the free's extents are not allocatable, be gone once txg
free + TXG_DEFER_SIZE has synced, not answer the free path asked for
plain-A's key again, and not survive an import; zdb must find the pool
clean.  On the previous commit and on the one before the minor fixes, at
ashift 12 and 9, `read` fails with ECKSUM and no redirect hit, and
`repair` returns plain-A's bytes rebuilt from O and rewrites the
overwritten column of O.

## Step 6: resilvers and txg-ranged scrubs walk the table

A resilver takes its window (min, max) from the DTLs, both bounds
exclusive, and the traversal prunes every block pointer born at or before
min.  A moved block keeps its birth t1 while N was written in t2, so one
with t1 <= min < t2 < max, a block moved while a child was missing, is
never reached; `dsl_scan_done()` then reassesses the DTLs and the child's
column of N stays stale.  The stopgap of the fixes above lowered min to the
oldest such t1, which traverses everything born since.

Each entry's value now also holds the block pointer the move found live and
the bookmark that named it (25 uint64s; only the five fields of the entry
are kept in memory).  `dsl_scan_visit()` starts with a walk of the table,
`dsl_scan_redirect_pass()`, before the DDT and the traversal: for each live
entry with t1 <= min < t2 < max, it checks the stored block pointer (it
must name the entry's key and pass `zfs_blkptr_verify()`, otherwise the
entry counts as one scan error), asks `dsl_scan_need_resilver()` about N at
t2, and reads the block with the stored pointer through `scan_exec_io()`,
as the scan's own I/O. `vdev_raidz_io_start()` sends it to N with `io_txg`
t2, so the children whose DTL holds t2 are rebuilt and the checksum
verifies the result.  Blocks with t1 inside the window are left to the
traversal, and a full scan (min 0) skips the walk.  The walk suspends with
the scan (`dsl_scan_check_suspend()`), and its cursor {state, vdev, offset,
birth} is synced with `scn_phys` under the pool directory key
`org.openzfs:spike_scan_redirect_cursor` and read back by
`dsl_scan_init()`, so the scan cannot complete, nor the DTLs be reassessed,
before the walk is done, and a resumed scan continues it.  Its reads are
waited for in the txg that issues them, and a block freed before the scan
of a txg is a tombstone the walk skips.  `spike_resilver_redirects` gains
`SPIKE_RESILVER_TABLE_PASS` (4); its default is now 6, the walk and
`vdev_raidz_need_resilver()` at N, with the stopgap (1) off and kept for
the comparison.  Test-only: `spike_scan_redirect_batch` (entries read per
txg), `spike_scan_redirect_verify`, `spike_scan_watch` and
`spike_scan_pass_stats`.  The resilver mode's `nowiden` (2) now also turns
the walk off, so `offline` still fails with it.

    crash_probe <dir> <ashift> rpass <R1|R2|R3|R4|R5|R6> <P1|P2> [none|stopgap|pass|both|noverify]

The setup is the resilver mode's plus 64 MiB of unmoved data written after
every block to move: the moved blocks hold about 1% of what is allocated.
R1 offlines child 2, moves plain, dedup, clone, gang and snapshot-only
blocks, brings the child back and resilvers; zdb must be clean, a scrub
must repair nothing, and a copy of the pool's files, with child 0 offline
and one more column of each N damaged, must read back every object.  R2
moves with every child online, damages a data column of each N and scrubs
txgs (epoch, now) with `spa_scan_range()`: each column must be rebuilt.  R3
reads one block per txg, exports after two, and the import must find the
resilver running and child 2's DTL still holding t2, resume from the cursor
and read the rest. R4 holds the walk (`zfs_scan_suspend_progress`), moves
late-L, frees the moved object the walk reaches next and lets it go: it
must meet the tombstone, read nothing at the extents the free released,
leave late-L (t2 after the window) alone, and R1's checks pass.  R5 adds a
mirror beside the raidz: the resilver must repair nothing on it.  R6
rewrites plain-A's stored block pointer to name vdev 99.

At ashift 12 and 9, under P1 and P2, R1 to R5 pass, and R6 passes under P2
with one scan error.  With `none` (no walk, no stopgap), and on the
previous commit with `spike_resilver_redirects` 2, R1 to R5 fail: the scrub
after the resilver finds 5 to 9 checksum errors on child 2 and repairs
them, the copy returns ECKSUM for the moved objects, the ranged scrub
leaves every damaged column as it was, and R3 has no walk to resume (in
three of four runs the resilver had finished before the export, in the
fourth it had not started).  With `noverify`, R6 fails an ASSERT in
`vdev_mirror_io_start()` in the sync thread.  The stopgap passes R1 and R5
too.  On R1 under P2 at ashift 12, the resilver with the walk examined 1.7
MB and visited 6346 block pointers in the window (58, 70); with the
stopgap, 86.4 MB and 13524 in (4, 70).  Both issued about 1.6 MB and their
leaves read 4.5 and 4.7 MB: raidz's and the mirror's need_resilver skip the
unmoved blocks by their DTLs, so here the stopgap costs traversal, not data
reads.  Beside a mirror (R5), the mirror's leaves read 0.1 MB or less
either way and nothing was repaired there.

Not covered: a checkpoint-aware move or free (the flags are computed, never
exercised), a frozen pool, a tombstone dropped early because a txg defers
no free (a nearly full pool), a scan I/O issued in the same sync as the
free (only a stale holder's read is exercised), a damaged entry whose N
overlaps another entry's extents, the stopgap's cost on a large pool, two
raidz vdevs with entries, a sorted scan interrupted while its queues are
not empty, a sequential rebuild, and kernel builds.  Step 7 takes up the
walk's cost on a large table and scale.

## Step 7: persistence and scale

The plan's targets, for 1.7e7 entries on one raidz vdev (a 20 TiB pool with
a tenth of its blocks pinned by snapshots): a lookup in 1 microsecond at
the 99th percentile, at most 1 GiB of memory for the table, and a table
load of at most 5 seconds at import.

    scale_probe <dir> <ashift> <command> [args] [<command> [args] ...]
    table_bench <avl|bt48|bt40|hash> <n> [threads] [rw|rrm|none]

`vdev_raidz_redirect_synth()` (test-only) adds synthetic entries in sync
tasks of any size, persisted as a move persists them, without their block
pointers (`ADD_HOT`, so that a full table fits a small disk; no test scans
such a pool) or in memory only, or removes them as frees do.  Each passes
the load's checks and has an old birth at an odd sector, where no parity-1
extent starts, so that no real block is redirected.  `scale_probe` drives
it on five sparse 4 TiB files (a 20 TiB raidz1 of 1279 metaslabs of 16 GiB,
512 MiB of incompressible data written at parity 1, then a parity-2
epoch): lookup latency through the I/O path's own lookup, one thread then
four (hits, misses at even sectors with old births, and new births), the
memory a table takes (malloc's in-use bytes, before and after it is
dropped), the import (with the `spa_tryimport()` that `zpool import` makes
first), the sync of a txg that adds or removes 1e5 entries, the on-disk
size, a read benchmark with `primarycache=none`, and the stress test
below.  It punches the pool's free space out of its files after each
persisted batch, so that the files take what the pool allocates.
`table_bench` builds candidate structures outside of any pool.

Conditions: ashift 12, a VM with 4 CPUs and 15 GB, the ARC capped at 2
GiB.  Until the commit after b026196, `scale_probe` set only
`zfs_arc_max`, which userland's `arc_init()` ignores below its
`arc_c_min` (half the default maximum): those runs had a 7 GiB ARC, and
every number below was measured again with the cap in force.  "Debug" is
this tree's build.  libzpool is always built with `-DDEBUG -DZFS_DEBUG`,
whatever configure says (`LIBZPOOL_CPPFLAGS` in `config/Rules.am`), so a
tree configured without `--enable-debug` changes nothing in it; "release"
below is such a tree whose libzpool, libnvpair and libzfs_core were made
with `LIBZPOOL_CPPFLAGS="-UDEBUG -DNDEBUG"` (zdb does not build that way:
it reads a `ZFS_DEBUG`-only field), and probes built to match
(`BUILD_TOOL_RELEASE=1`).  Both use `-O2`.

The previous design (an AVL node of 120 bytes per entry under one rwlock,
a ZAP of 25 values per entry, loaded with a cursor and a lookup per entry)
missed every target.  With 1.7e7 entries in memory (5e5 of them loaded
from disk), a lookup took 4.7 to 6.2 microseconds at the 99th percentile
(2.6 to 2.9 at the median), and the table 128.0 bytes per entry, 2.03 GiB.
The load took 1.30 to 1.48 s for 2e5 entries and 5.9 to 6.8 s for 5e5
(6.5 to 7.4, then 11.9 to 13.7 microseconds per entry), and `zpool
import` paid it twice, since its tryimport loaded the table too: 3.0 to
3.2 s, then 12.7 to 14.3 s, in all.  At 12 microseconds per entry, 1.7e7
entries would take about 200 s per load (an extrapolation from the 5e5
runs; the cost per entry grew with the table).  An entry took 448 to 477
bytes of ZAP, 1724 to 1805 allocated (the MOS keeps three copies), so 1e6
entries already passed the 4.6 GB these tests allow for pool files; a txg
adding or removing 1e5 entries synced in 4.2 to 7.0 s (5e5 on disk, 1.7e7
in memory).

The table is now:

* in memory, per raidz vdev, 1024 open-addressing tables (one per metaslab
  modulo 1024), linear probing in Robin Hood order with backward-shift
  deletion, of 40-byte slots: offset, birth, new offset, and the new birth
  as a 32-bit delta and the three sizes in 512-byte units.  A shard that
  reaches 90% use grows to two thirds; the load leaves each at 80%.  The
  key stays {offset, birth}: the kept head sector makes the offset unique
  among live entries and tombstones under P1 and P2, but not under the
  test-only P3;
* read without a lock: a reader counts itself in one of 64 counters, a
  cache line each (by CPU in the kernel, by a hash of the thread here), and
  checks that no writer is in; a writer (syncing context or the load) holds
  the rwlock, raises a flag and waits for the counters to drain before it
  moves a slot or frees an array, and a reader that finds the flag waits on
  the rwlock.  No writer section spans an I/O.  A real implementation would
  rather use RCU or SMR;
* skipped by a lookup when the table is empty, when the birth is later than
  every entry's (every block of the last parity epoch, new writes included)
  or when the metaslab's shard is empty;
* on disk, a log of 40-byte records (an entry added, a key removed) in a MOS
  object whose bonus holds the record count and how many of the first are
  sorted, beside a ZAP keyed like the entries that holds each one's block
  pointer and bookmark, which only the scan reads.  The move appends an ADD
  and adds the block pointer, the free's txg appends a REMOVE and removes
  it, in the txgs they did before.  When the records pass twice the entries
  (plus 65536), or the unsorted tail outgrows the sorted part, the next
  sync of the vdev rewrites the log in key order, one record per live
  entry, in a new object that replaces the old one in the same txg.  The
  load never runs it, but the first txg after the load, which
  `spa_import()` itself waits for, does: an import that finds the log due
  takes the condense too (below).  A table of the earlier format is
  refused (ENOTSUP, by read-write and read-only imports alike);
* loaded by reading the log in windows of 2^20 records, sorting each by
  shard (stably: the records of a key stay in order) and replaying the
  shards' runs in parallel tasks, then trimming each shard to 80% use; a
  tryimport skips the load (it reads only MOS blocks, never moved).  Each
  ADD is checked as before, and also a record of unknown type, an ADD of a
  key the table holds, a REMOVE of a key it does not hold, and a header
  that counts more records than the object holds, with the same answers: a
  read-write import fails with EROFS, a read-only one leaves the record
  out;
* walked by the scan metaslab by metaslab, skipping a shard whose entries
  were all moved before the window, from a sorted copy of the metaslab's
  keys in the window, kept while the walk is in it.

With 1.7e7 entries loaded from disk:

| | target | previous design | this design, debug | this design, release |
| --- | --- | --- | --- | --- |
| lookup p99, hit or miss, 1 thread | 1 us | 4.7 to 5.6 us | 0.63 to 0.77 us | 0.62 to 0.68 us |
| lookup p99, hit or miss, 4 threads | 1 us | 5.8 to 6.2 us | 0.55 to 0.61 us | 0.55 to 0.64 us |
| lookup p99, birth after every entry's | | 4.7 to 5.9 us | 44 to 64 ns | 41 to 53 ns |
| memory | 1 GiB | 2.03 GiB | 0.79 to 0.84 GiB | 0.79 to 0.82 GiB |
| table load at import | 5 s | about 200 s, twice (extrapolated) | 2.44 to 3.24 s | 2.49 to 2.98 s |
| whole import, tryimport included | | | 2.65 to 3.45 s | 2.68 to 3.18 s |
| txg adding / removing 1e5 entries | | 4.6 / 4.2 to 7.0 s | 2.07 to 2.22 / 1.70 to 1.80 s | 0.92 to 0.97 / 0.55 to 0.58 s |

Every target is met in both builds: the lookup by 23 to 45%, the memory
by 16 to 21%, the load by 35 to 51%.  The previous design missed them by
4.7 to 6.2 times, 2 times and, extrapolated, about 40 times (80 counting
the tryimport's load).  The lookup is bound by memory, not by the
assertions: the release build is no faster.  At the 99.9th percentile it
took 0.9 to 4.3 microseconds in both builds.

The memory is 50.0 bytes per entry after a load of a log in key order
and 51.8 after one in random order (52.8 for a log of which 1.5e7 records
were sorted), malloc's in-use bytes freed when the table is dropped.  An
import spends 0.075 to 0.12 s in its find and tryimport, which loads
nothing, and 0.09 to 0.13 s beside the load; the load spends 1.2 to 1.6 s
in `dmu_read()`.  The same holds with the log in key order or in random
order (2.44 to 2.99 s and 2.73 to 3.24 s of load in the debug build), and
after `echo 3 > /proc/sys/vm/drop_caches` (the guest's page cache went
from 1.2 to 3.1 GB to 0.19 GB; the host may still have cached the virtual
disk, which these runs cannot tell).  A log that the test had kept from
condensing (`SCALE_CONDENSE=-1`) was due at import: that import took 7.7
s, 3.2 s of load and a condense of 2.2 s in the txg `spa_import()` waits
for.  Under the default policy the log is condensed in the txg that makes
it due, so an import should find it due only after an export or a crash
between that txg and the next sync of the vdev: reasoned, not run.  A
condense of 1.7e7 entries takes 1.9 to 2.2 s and its txg syncs in 4.3 to
4.8 s, with both logs on disk until then (3.6 to 3.7 GB of pool files,
against 2.2 GB before).  The log takes 40 bytes per entry, 72 allocated at
1.7e7 entries (61 to 64 at 1e5); the block-pointer ZAP, measured at 1e5
entries, 362 to 364 bytes, 1482 allocated at ashift 12 and 814 at ashift
9, which would make 25 GB at ashift 12 for 1.7e7 real moves (an
extrapolation: synthetic entries at that scale are persisted without
their block pointers, which would not fit this disk).

In `table_bench` at 1.7e7 entries, in the debug build, a btree of 40-byte
elements took 58 bytes per entry and 2.5 to 2.7 microseconds at the 99th
percentile, and a hash table behind one rwlock 1.5 microseconds at the
99th percentile with four threads (0.6 to 0.7 with no lock): that is why
the shards are hash tables read without a lock.

What the table costs a raidz read it does not redirect, measured on 512 MiB
of 16K blocks read with `primarycache=none` from a warm page cache, every
pass with no redirect hit: each variant ran 8 times, alternated with the
base commit 94407d4 in 4 rounds of one order and 4 of the reverse, and is
compared with the base run of its own round.  In the debug build (the
base, the previous design with an empty and a full table, this one with an
empty and a full table), with one thread every variant is within 1.5% of
the base's 108 microseconds of wall time per block, and none of the
differences is resolved (|t| at most 1.1 over 8 rounds); with four threads
the variants read 1.3 to 3.1% slower than the base's 31.7, and only this
design's full table stands out at all (+3.1% wall, t = 2.3; +2.6% CPU, t =
2.5).  In the release build (the base, this design empty and full), the
variants are within 2.6% of the base's 101 and 29.1 microseconds, none
resolved (|t| at most 1.5).  Read through files in userspace, a block
costs about 100 microseconds in either build, so the lookup's own cost, a
third of a microsecond on average in the microbenchmark, is below what
these runs can resolve.

    scale_probe <dir> 12 create 64 synth 0 100000 mem 100000 stress 120 3 500000

The I/O path reads the shards without a lock while syncing context moves
their slots and frees their arrays.  `stress` keeps reader threads looking
up the stable entries, and keys in the same metaslabs, while the sync
thread adds five times as many keys in one sync task, removes them in
another and lets their tombstones expire, round after round: every shard
grows, shrinks, shifts its slots and is condensed under the readers.  A
stable entry must always be found with its fields, and a key found must
carry its own.  The test-only `spike_redirect_unsafe_readers` makes a
writer skip the wait for the readers, as a table without the protocol
would (`SCALE_UNSAFE`).  In 120 s with three readers, without the
protocol, they missed 1020 stable entries and read 4 wrong ones in 2.6e9
lookups (and 1147 and 3 in 3.1e9 in an earlier run); with it, none in 2.0e9
lookups (1.9e9 earlier).  The wait has a cost for writers: with the
readers looking up without a pause, the sync tasks that add and remove
5e5 keys took 13.7 and 20.7 s per round, against 6.0 and 6.3 s without
the protocol.  Every insert and removal is its own writer section; the
I/O path looks up far less often than these readers do.

    crash_probe <dir> <ashift> move <point> <P1|P2> condense
    crash_probe <dir> <ashift> fcrash <point> <P1|P2> [condense|condensebad]
    crash_probe <dir> <ashift> corrupt <kind> [P1|P2] [lax]

`condense` rewrites the log in the txg of the move, or of the free
(`spike_redirect_condense` 1 and 2); at every crash point the import must
find the state before or after, and at POST_UB a log in key order with one
record per entry.  `condensebad` keeps the freed entries in the rewritten
log (`spike_redirect_condense_tombs`): at POST_UB the freed block's entry
comes back.  The corrupt mode now damages plain-A's record in the log;
`rtype` (an unknown type) and `rdup` (a second ADD of its key) replace
`ksize` and `vsize`, and `nasize` and `rmisalign`, sizes misaligned by half
a sector, cannot be written at ashift 9, where the log's 512-byte units are
a sector.  `lax` (`spike_redirect_load_lax`) skips the load's checks of
the log's structure.  `scale_probe`'s other switches: `SCALE_LOAD_SKIP`
(`spike_redirect_load_skip`, an empty table), `SCALE_CONDENSE` and
`SCALE_UNSAFE`.

At ashift 12 and 9, on the commit that adds the stress test, 252 runs of
the probes: 227 pass, and the others are deliberate demonstrations and two
runs that cannot be written.  redirect_probe's checks pass (2 of 2), T4a
under P1 and P2 (4), T4b under P1, P2 and P3 and P1 `nofix` (8), the move
and free crash matrices (32 each, half of them with `condense`), the free
cases (22), resilver (17, `replace nowiden` included), stalebp (8, none
skipped), guards (16), corrupt (52), tomb (4), converge (8) and rpass R1 to
R6 (22).  `condensebad` fails at POST_UB in all 4 runs (8 redirects
loaded, 7 expected: the freed block's entry is back), `rtype` and `rdup`
with `lax` fail in all 4 (the read-write import takes the damage, and
`rtype` then aborts in the range tree), and the earlier steps'
demonstrations fail with their signatures (T4a under P3 and P3 `alone`,
T4b `nofix` under P2 and P3, the free of P3 with `nocheck`, the resilver
switches, R6 `noverify`); `nasize` and `rmisalign` skip at ashift 9.  A
60 s ztest of raidz exits 0.

Not covered: a kernel; a release build of zdb (it does not compile without
`ZFS_DEBUG`); the scan walk's cost on a large table (it is bounded by the
shards moved in the window, by construction; synthetic entries have no
block pointer a scan could verify); the block-pointer ZAP at 1.7e7
entries; a condense spread over txgs (one rewrites the whole log in one
txg); an import that finds the log due under the default policy; two raidz
vdevs; I/O costs outside this userspace file-backed setup; and pools of
the earlier spike formats, which are refused.

## Fixes: moved blocks with a second copy

A block pointer can hold several DVAs: data written with `copies=2`,
metadata under `redundant_metadata`, and every gang header, which gets one
copy more than its data.  Their I/O goes through the root mirror
(`vdev_mirror_map_init()` with no vdev), which reads one DVA and, for a
resilver, repairs each other DVA whose top-level vdev's `DTL_PARTIAL`
holds the block's birth; `zio_vdev_io_start()` then bypasses a repair
write on any vdev whose `DTL_PARTIAL` does not hold the write's `io_txg`.
A DVA that a raidz vdev moved is read and repaired at N, written in t2, so
its DTLs hold t2, not the birth t1.  A resilver read that the root mirror
served from the other copy (a random choice when the copies are on
different top-level vdevs) therefore left N's column on the returning
child stale, and `dsl_scan_done()` still cleared the child's DTL: the
block lost a level of parity without a trace.  The walk and the stopgap's
traversal both read through that path.

The root map now keeps, for each child, the txg its copy was written in
(`vdev_mirror_child_txg()`: the new birth of a moved raidz DVA, otherwise
the block's birth), and uses it to pick the child to read, to choose the
children to repair, and as the `io_txg` of the repair write.  Only reads of
blocks with more than one copy look it up.  Test-only:
`spike_mirror_redirect_txg = 0` uses the birth again, and
`spike_mirror_redirect_repairs` counts the repairs of moved copies, and of
gang headers among them.

    crash_probe <dir> <ashift> dva2 <P1|P2> [nomirror|stopgap|nofix]

A mirror of m0 and m1 sits beside the raidz before any data is written;
32 plain objects are written with `copies=2`, then 8 gang objects with
`copies=1`, whose headers get two copies.  Each block whose DVA 0 is on the
raidz and DVA 1 on the mirror is moved (the gang headers, and those of
their children on the raidz) while child 2 is offline, with the mirror
`noalloc` so that N lands on the raidz; then the child comes back and the
resilver runs.  zdb must be clean, a full scrub must repair nothing, and on
a copy of the pool's files, with DVA 1 of each moved block damaged on both
legs and two columns of each N damaged on children other than 2, every
object must read back, which parity 2 allows only if the resilver rebuilt
child 2's column.  `nomirror` keeps both copies on the raidz (no copy
read), `stopgap` sets `spike_resilver_redirects` to 3, so that the
traversal reads the moved blocks instead of the walk, and `nofix` clears
`spike_mirror_redirect_txg`.

At ashift 12 and 9, under P1 and P2, 33 (ashift 12) or 29 (ashift 9)
blocks with two copies were moved, 49 or 45 moves with the gang children.
On the previous commit, the default and `stopgap` fail in all 8 runs: the
scrub after the resilver finds 11 to 22 checksum errors and repairs 0.3 to
0.8 MB, and as many objects return ECKSUM from the copy, gang objects among
them in 4 runs.  With the fix and `nofix`, the 4 runs fail the same way
(12 to 17 objects, gang objects among them in 3).  With the fix, the 8
runs pass: the root mirror repaired 12 to 20 moved copies from the other
copy in each resilver, 2 to 6 of them gang headers.  `nomirror` passes on
both: with both copies on one vdev, `vdev_mirror_dva_select()` prefers
DVA 0, the moved one.

A sorted scrub queues each DVA of a block as its own I/O, with that DVA
first, and the root mirror of such an I/O reads only its first DVA
(`vdev_mirror_map_init()`, `c = 1`): the other DVAs come as their own
I/Os.  Since step 4, `dsl_scan_enqueue()` issued a block with any moved
DVA whole and at once, since the queues cannot place a moved DVA, so a
sorted scrub read DVA 0 of such a block and never checked its other
copies.  Each DVA is now routed on its own: a moved one is issued at once,
as its own I/O with that DVA first (the order `bp2sio()` gives a queued
one) and counted alone as issued, and the others are queued as before.
`spike_scan_redirect_split = 0` (test-only) issues the whole block again.

    crash_probe <dir> <ashift> dva2scrub <P1|P2> [nomove|nofix]

The setup is dva2's; the marked blocks are moved with every child online
(not with `nomove`), the pool is exported, the first sector of each one's
DVA 1 is inverted on mirror leg m0, and a scrub runs, which must be sorted
and must bring each damaged sector of a plain block back to m1's bytes.

At ashift 12 and 9, under P1 and P2, 25 (ashift 12) or 21 (ashift 9) plain
blocks were damaged.  On fd0cdf8 and on the commit before this one, the
scrub found none of them in all 8 runs: no checksum error on m0, every
sector still damaged.  With this commit it found and repaired all of them
(25 or 21 checksum errors on m0), and with `nofix` none again.  `nomove`
passes on all three.  In every run, moved or not, the scrub repaired none
of the 8 damaged gang header copies: a sorted scrub issues a gang block
whole, the header's read inherits `ZIO_FLAG_SCRUB`, and the root map of a
sorted scrub's read keeps only DVA 0 (`c = 1` in `vdev_mirror_map_init()`),
since it expects each other DVA to come as an I/O of its own, and a gang
block's never do.  None of that is the spike's, and the base and upstream
behave the same:

    gang_scrub_probe <dir> <ashift> setup <rzmirror|single> [c2gang]
    gang_scrub_probe <dir> <ashift> damage <others|first>
    gang_scrub_probe <dir> <ashift> scrub <sorted|legacy>
    gang_scrub_probe <dir> <ashift> check

`gang_scrub_probe.c` uses nothing of the spike, so `build-tool.sh` builds
it against any tree of this era, crash_probe's included.  `rzmirror` is
dva2's layout without the parity-2 epoch, `single` one file vdev; it
writes dva2's objects (with `c2gang`, 8 gang objects with `copies=2` as
well: three header copies, two per child), inverts the first sector of
each DVA but DVA 0 of a block with several (`others`, or DVA 0 with
`first`) where a file can reach it, scrubs, sorted or with
`zfs_scan_legacy=1`, and `check` exits 1 unless every sector is restored.

On 94407d4 (no spike code), 65e6ce5 and upstream master c27dff0, at
ashift 12 and 9 on `rzmirror` and 12 on `single`, and on 1d9eb6d,
84c3d6b, fd0cdf8 and 3223bef at ashift 12 on `rzmirror`, a sorted scrub
restored every plain block's damaged copy (25 on `rzmirror` at ashift 12,
21 at ashift 9, 32 on `single`) and none of the 8 gang header copies.  A
legacy scrub restored all of them on the first three, and on master after
a sorted scrub of the same pool had left the header copies damaged.  On
master with `c2gang`, the header copies (16 on `rzmirror`, 24 on `single`)
and the children's second copies (6 and 16) behave the same, and with
`first` a sorted scrub restores the damaged DVA 0 of every header and
child (16 and 16 on `single`): their other copies are read only once DVA
0 fails.  zdb shows each damaged sector to be DVA 1 of a gang block
pointer (`G`), in this probe's `rzmirror` pool and in crash_probe's
dva2scrub pool.  The gap is upstream's: neither a regression of the spike
nor a fault of the probe.

Neither fix changes the table, its lookup, its memory or its load, so
step 7's measurements were not run again.  Fix 1 adds a lookup, skipped
like the I/O path's when no entry can match, to each read of a block with
several copies; its cost on reads was not measured.

## Step 3A: encrypted datasets without their keys, and every DVA

    crypt_probe <dir> <ashift> [nodva] [noroot] [noraw] [nomos] [keep]

`crypt_probe.c` builds a raidz1 pool with plain, dedup, gang, embedded and
`copies=2` blocks, an encrypted dataset (two snapshots, blocks only a
snapshot holds, a clone, `copies=2`, dedup and gang blocks) and a raw full
and incremental receive of it into a second encryption root, then adds a
parity-2 epoch.  With every key unloaded, it rewrites the MOS by dirtying
its old blocks in a sync task (the table lives in the MOS and cannot
redirect it), then moves every other old DVA, found by a traversal with
`TRAVERSE_NO_DECRYPT`.  A census counts each distinct DVA as new, moved or
old, and a DVA oracle reads every DVA alone, raw, against its checksum, on
ten copies of the pool, each with another pair of children inverted: a
parity-1 extent cannot survive every pair, a parity-2 extent does.

Three changes to the move were needed, each with a switch that brings its
failure back:

* `vdev_raidz_redirect_move()` takes `rdm_dva`, the DVA of the block
  pointer to move (`nodva` moves DVA 0 only, as before): 107 DVAs stay old,
  every second copy and the third copy of metadata;
* the move check refused every bookmark of object 0 as a ZIL bookmark
  (`ZB_ZIL_OBJECT` is 0, the meta-dnode's and the root's object), and the
  liveness check could resolve neither an objset's root block nor a block
  of its meta-dnode; it now checks a root against `ds_bp` and a meta-dnode
  block through `DMU_META_DNODE()` (`noroot`, `spike_redirect_live_objset
  = 0`): 105 stay old;
* the liveness check found a block below an indirect one with
  `dbuf_dnode_findbp()`, whose `dbuf_read()` decrypts the dnode block
  first and fails with EACCES without the key; it now reads indirect
  blocks with `DMU_READ_NO_DECRYPT` (`noraw`, `spike_redirect_findbp_raw =
  0`): 120 encrypted level-0 blocks are refused with ESTALE.

At ashift 12 and 9, the default passes: 360 DVAs outside the MOS moved
with no key loaded (138 encrypted level-0, 14 authenticated level-0, 78
indirect with MAC, 249 held by a snapshot, 94 raw-received, 24 of the
clone, 4 gang headers and 4 children, 2 dedup, 98 second and 9 third
copies), the MOS rewritten in one round, no DVA left old, every object
bit-exact with the keys loaded, zdb -bcc clean, a scrub with no key
loaded repairs nothing, and every one of 662 (ashift 12) or 668 (ashift 9)
DVAs reads under each of the ten pairs.  Before the move, 460 to 476
DVAs fail under some pair at ashift 12, and at ashift 9 no damaged copy
imports.  Each switch, and `nomos`, leaves a residual of 105 to 120 DVAs
that the census counts, and the DVA oracle fails every such run: it finds
the same number for `nodva` and `noraw` at both ashifts and for `nomos`
and `noroot` at ashift 12; at ashift 9, `noroot`'s damaged roots hide
whole datasets from some copies (42 DVAs fail, 106 are not met under every
pair) and no copy without the MOS rewrite imports.  The object oracle
(children 1 and 3 only) passes `nodva`, `noroot` at ashift 12 and `nomos`
at ashift 12: a small block can miss both children, and one surviving copy
is enough to read an object, which is why the DVA oracle exists.

The move phase also prints what the raidz holds against what the DSL
charges, before and after the move, once the deferred frees have synced.
A moved block keeps its DVA, so its dsize, while the vdev now holds N and
the kept sector instead of the old extent.  At ashift 12 the vdev's
allocated space grew from 40.5 to 55.2 MB while the DSL's used space grew
by 0.9 MB (the table's own MOS objects): the vdev holds 10.9 MB (deflated)
more than any dataset is charged, for 26.8 MB of moved blocks (9.2 MB for
26.4 MB at ashift 9).  "available" overstates the free space by that much.
Not fixed: a real implementation would charge the deflated difference to a
counter persisted with the table, subtract it in `spa_update_dspace()` and
release it at free.
