# Raising RAID-Z parity in place: joint design

Working document for one design, written jointly by @rdmitry0911 and
@tigerblue77, before anything is proposed upstream. Discussion:
https://github.com/tigerblue77/zfs/issues/2

How this branch works:

- This file is changed only through pull requests into `parity-joint`,
  reviewed by the other author.
- Each prototype comes in as its own pull request, for side-by-side
  review. Prototypes are not merged here; only this document is.
- Anything stated as "mine does X" is "mine does X in that fixture"
  until the other side has run it.

## 1. Problem and constraints

Writes: tigerblue77. Reviews: rdmitry0911.

Draft by tigerblue77, 26 October. The goal and the four constraints are
those of #2, which rdmitry0911 signed "as written" on 21 October. The
requirements after them collect what each author has since asked of
either mechanism; section 3 holds the choices of how to meet them.

**Problem.** The parity-epoch table of openzfs/zfs#19262 writes the new
blocks of a RAID-Z top-level vdev at a higher parity, but the blocks
written before keep the old one, and the vdev reports the lowest parity
of any layout it has held (`vdev_raidz_min_nparity()` in that series).
Its tolerance therefore does not change. amotin's 16 October review of
openzfs/zfs#19262 asked to "first present/discuss what and how you are
going to implement, then implement it"; this document is meant as that
presentation.

**Goal.** Raise the parity of an existing RAID-Z top-level vdev online,
at the same width, and report the new tolerance only once no DVA on it
is left at the old parity.

**Constraints.**

- No narrowing, no lowering. Narrowing in place is impossible
  (openzfs/zfs#9013). A parity change combined with an added disk is out
  of scope.
- No block pointer rewrite. Snapshots are immutable: no block pointer,
  checksum, MAC or IV changes.
- Rewrite alone is not enough. It cannot reach blocks that only
  snapshots hold, or encrypted data whose key is not loaded, and one
  block left behind voids the guarantee (openzfs/zfs#19233). That is why
  openzfs/zfs#19419, which proposes to rewrite every block of a vdev,
  refuses when snapshots or a checkpoint hold blocks.
- Padding can grow a block from 4 sectors to 6 (openzfs/zfs#16677), so
  data may not fit even with enough raw space.

**Requirements on either mechanism.**

1. Completeness: the new tolerance is reported, and the old epoch
   retired, only from a durable record written after a census of zero
   that the allocator can answer, pending frees and asynchronous
   destruction included (point 4 of section 3).
2. Sharing: dedup and block-cloning references survive the move, and a
   block that several references share is copied once.
3. Lifetime: an old extent is reused only once no reader can still be
   reading it, ordinary reads included; a defer interval alone does not
   prove that (point 8).
4. Repair: each copy is checked against the DTLs at the txg it was
   written in, and a resilver visits every copy written inside its
   window, whatever birth its block pointer carries.
5. Space: dataset dsize never changes between a block's birth and its
   free; what the move adds is charged where the allocator refuses on
   it, and a move once started can finish (points 6 and 12).
6. Recovery: an import after a crash at any point finds the state before
   or after each step of the move.
7. Size: the mapping's memory is estimated before a move starts and held
   to a budget (point 3).

## 2. The two mechanisms

Writes: rdmitry0911; tigerblue77 corrects the column describing the
redirect prototype. Each reviews the other's column.

## 3. Decision points

Writes: tigerblue77. Reviews: rdmitry0911.

Draft by tigerblue77, 26 October. It merges the six questions of #2, the
points of the 25 October reply (rewind, readers in flight, dedup, BRT,
expansion) and the open decisions rdmitry0911 has named on #2, in #4 and
in #5; the table at the end shows where each went. rdmitry0911's own
list of nine has not been posted, so he may add any of it that is
missing. Each point gives the question, the options, where each
prototype stands and what would settle it. "Redirect" is the prototype
of #3 and "virtual/backing" the one of #4; their positions describe the
code as published, not the converged design.

References: `spike:` is `spike/bypass-free` at `094d2a05f` and `vb:` the
head of #4 at `46d04a1af`. A path without a directory is under
`module/zfs/`, except `README.md` and redirect's `*_probe.c`, which are
under `contrib/raidz-redirect-spike/` (`spike:`) or
`contrib/raidz-virtual-backing/` (`vb:`). A bare `file:line` is upstream
master at `ea111ca50f`.

### Proposed by both authors, for the maintainers to confirm

**1. A mapping beneath unchanged block pointers.** Is a per-block
mapping from a block's old extent to a copy at the new parity acceptable
for the blocks a rewrite cannot reach? It is device removal's principle
("Since we cannot rewrite block pointers of snapshots, etc., we keep a
mapping", `vdev_indirect.c:37-39`), per block because RAID-Z needs block
boundaries. Both prototypes rely on one; rdmitry0911 accepted it on 21
October on the condition of point 4.

**2. Which blocks get a row.** Proposed default, subject to section 4:
blocks that a head whose key is loaded owns alone are rewritten through
the normal write path, logical birth kept; snapshot- and clone-held,
dedup- and BRT-shared and keyless blocks are mapped; new and untouched
blocks get no row. rdmitry0911 is "open to the hybrid being the
default" (26 October) if section 4 shows fewer retained rows and lower
full memory on head-heavy cases, without losing sharing or failing the
lifetime, recovery and space checks, and with acceptable copy work,
latency and fragmentation (#5). The alternative, his position of 21
October, makes the mapping the general mechanism and the rewrite an
optional drain; as the 25 October reply on #2 argued, unmodified head
data then keeps its rows, and the table is bounded by capacity rather
than by retention. Neither prototype is that default yet:

- Redirect: the hybrid is a probe (`spike:hybrid_probe.c:557-562`,
  `774-818`). On master a rewrite of a dedup block is a no-op
  (`zio.c:4364-4387`), so the census round after it redirects the block;
  not witnessed under the hybrid. A clone's block pointer takes the
  clone's txg as logical birth (`dmu.c:3033-3035`), so the ownership
  test rewrites, and un-shares, BRT-shared head blocks until what
  `brt_maybe_exists()` flags (`brt.c:905`) is redirected instead; not
  built. Where the checksum and compression allow nopwrite
  (`dmu.c:2706-2708`), a rewrite of unchanged data keeps its old block
  (`zio.c:3751-3778`), so the rewrite needs an exemption; not built, and
  the hybrid fixture has compression off (from reading, not tested).
- Virtual/backing: new allocations are paired too
  (`vb:README.md:9-11`); identity mapping without those rows "remains a
  change to build and measure" (26 October).

**3. A table bounded by retention, with an estimate and a budget.** An
entry leaves when its block is freed; nothing drains the table by
rewriting snapshot metadata, which both authors agree is block pointer
rewrite. Before a move starts, an estimate, as `zpool remove -n` prints
one for removal, and a refusal above a tunable budget (Q2 of #2, agreed
21 October). The budget is to be agreed from section 4 (26 October).
For reference: about 50-100 MB per TB for device removal's mapping
(ahrens on openzfs/openzfs#627, as quoted in #2); 50.0-52.8 B per entry
in RAM for the redirect table at 1.7e7 synthetic entries
(`spike:README.md:566-568`), whose per-entry block-pointer ZAP for the
scan walk (1482 B allocated at ashift 12, `spike:README.md:583-588`)
must go; no complete figure yet for virtual/backing
(`vb:README.md:117-121`). Snapshots kept for years keep their rows; an
in-pool `zfs send -R | zfs recv` can empty them.

**4. The completeness gate.** The new tolerance is reported, and the old
epoch retired, only from a durable record written after a census of zero
(Q3 of #2). The census must be one the allocator can answer rather than
a walk's own report (rdmitry0911's condition of 21 October, accepted on
25 October), pending frees and asynchronous destruction included.
Options:

- Reconcile the mapping with the native space maps. Virtual/backing
  certifies only with no legacy bytes, no current or retired version
  below the target parity, a recorded floor and no checkpoint
  (`vb:vdev_reloc_convert.c:437-460`); the certificate then raises the
  parity the vdev reports (`vb:vdev_raidz.c:2247`).
- A per-vdev count of old-epoch allocated bytes kept by the allocator,
  decreased by every free and move that releases them; built in neither
  prototype.
- A traversal, which is what redirect has today, in test code
  (`spike:crypt_probe.c:23-27`, `1123-1142`). One probe counts the
  deferred and free bpobjs beside it and only reports an async-destroy
  bptree; a walk is not enough alone.

The guard proposed with it in #2, not built, as rdmitry0911 corrected
it on 27 October: once an epoch is retired, refuse a dispatch, free or
claim that would use its obsolete physical layout without a valid
mapping. An unchanged block pointer may still carry a birth in that
epoch, so its mapping is validated and its copy resolved before the
guard applies (redirect: the entry keyed by offset and physical birth;
virtual/backing: the allocation identity, whose dispatch keeps the
block pointer for verification and takes the copy's offset, size and
birth, `vb:vdev_reloc_engine.c:1923-1937`). Read, repair, free and
claim of that block remain valid. An absent or stale entry must never
fall back to the old layout at the old extent, nor reach another block
that has reused the address.

The durable record needs validity rules of its own. A change that could
make any reachable block's backing weaker than the record states is
refused, or durably withdraws the record before that state is
published; only a new record, after another census of zero, restores
the new tolerance. Keeping the feature active (point 5) is a separate
obligation.

**5. One feature.** Epochs, mapping and census form one pool feature,
not read-only compatible, since older code would read the old extent;
it is registered only in the last step of section 5 (Q3 of #2, agreed
21 October). The epochs of openzfs/zfs#19262 lose their own feature and
land with this one, as rdmitry0911, their author, agreed on 26 October;
the experimental GUID of #19262 is not reused with a meaning that an
epochs-only reader would accept. The feature is active before the first
incompatible state becomes durable: an epoch table, a mapping row, or
bootstrap or journal state if the design has them (points 7 and 11).
Its guard is visible before any translated MOS read, at every crash
and import point, so that older code refuses read-only imports as well
as read-write ones. It stays active while any durable state on an
affected vdev, or in a retained or recoverable root, still needs it:
an empty mapping is not enough, since epoch layouts, a checkpoint,
retained copies, a journal and a durable floor or completion record can
outlive the last row. A first implementation may stay active for good;
deactivation needs its own proof that every such root is fully readable
by older code. Activation is separate from the durable record of
point 4, which alone lets the new tolerance be reported. Neither
prototype registers a feature for its mapping (redirect: none;
virtual/backing: a private label entry that older code refuses,
`vb:spa.c:5017-5020`, and "no supported administration interface or
agreed integrated feature/format", `vb:README.md:15-17`).

**6. Space, the parts agreed.** Dataset dsize never changes between a
block's birth and its free. What a move adds is charged at pool level,
as the number the allocator refuses on rather than a display
subtraction, and it holds for the whole move (Q4 of #2, with
rdmitry0911's two additions of 21 October, accepted on 25 October).
Redirect charges it in `spa_update_dspace()`
(`spike:spa_misc.c:2117-2127`), which the DMU's space check reads
(`dsl_pool.c:950-951`); virtual/backing admits each transaction against
physical credits and a standing reserve (`vb:vdev_reloc_admit.c:1-12`).
The open parts are point 12.

### Open between the authors

**7. Where the mapping lives.**

- In the MOS (redirect): a log object and a block-pointer ZAP per vdev,
  loaded right after the epoch table and before any block outside the MOS
  is read (`spike:vdev_raidz.c:8924-8927`). MOS blocks are then never
  mapped (`spike:vdev_raidz.c:8343-8352`) and must be rewritten through
  the normal write path in syncing context, as one probe does
  (`spike:crypt_probe.c:38-41`).
- In raw replicated regions on the vdev's own children, found from an
  uberblock extension before the MOS is read (virtual/backing; rdmitry0911
  on #2, 21 October). MOS blocks are mapped like any other, at the cost of
  new fields in `struct uberblock`
  (`vb:include/sys/uberblock_impl.h:175-176`) and of metadata outside the
  MOS.

Is an uberblock extension with raw per-child metadata acceptable, or
must the mapping live in the MOS?

**8. Old extents and readers in flight.** Requirement 3 of section 1
holds for both; the options differ in when an old extent is reused.

- Free most of it at the move (redirect). One sync task writes the copy
  and waits, adds the entry, then frees the old extent's tail as an
  ordinary deferred free (`spike:vdev_raidz.c:8492-8510`); scan and
  resilver I/O are waited for within their txg (`dsl_scan.c:4739-4758`,
  `4790-4794`). Ordinary reads rely on the defer window alone
  (`spike:vdev_raidz.c:2875-2882`), and a txg that defers no frees (free
  space at or below the slop, `metaslab.c:4632-4637`) makes the tail
  reusable at once; the move does not pause there. Proposed, not built:
  expansion's `vre_rangelock`, which RAID-Z I/O takes as reader only
  during a reflow today, taken as reader by every RAID-Z I/O while a move
  runs and as writer by the move over the old extent before its tail is
  freed (`vdev_raidz.c:2844-2848`, `4609-4610`), and a pause while frees
  are not deferred. Cost: tails that cannot hold a block of the same size
  at the new parity, for as long as the block lives
  (`spike:README.md:935-941`).
- Retire the old extent whole, and reuse it only once the leases on it
  have drained and a durable fence has passed (virtual/backing): "a read
  that meets a reused extent fails ESTALE and retries through the
  catalog" (21 October). Cost: up to one batch of retired bytes until
  the fence (64 MiB in its fixture, 21 October), and the floor of point
  9.

Settled by section 4, item 5: an ordinary read held across the reuse
boundary, the no-deferral case included, in both prototypes.

**9. Rewind.** `-F` reaches back at most `TXG_DEFER_SIZE` txgs
(`spa.c:6583-6585`), which an ordinary deferred free covers. `-X`, and
`-T`, which implies it (`cmd/zpool/zpool_main.c:4470-4472`), guarantee
no consistency (`zpool-import.8`) and can reach an old extent whose
freed part another block now holds: a moved block then fails its
checksum, as after any free, except that a move frees part of every
block it moves. Options: stock behaviour, or a durable floor that
refuses, in every import mode, roots older than released backing
(virtual/backing). Both authors keep the floor policy open (26 October).

**10. Checkpoint.**

- Refuse a move while a checkpoint exists, as attach and removal refuse
  one (`spa.c:8500-8504`, `vdev_removal.c:2487-2489`; redirect:
  `spike:vdev_raidz.c:8329-8330`). A free after a later checkpoint is
  checkpointed by each extent's own birth
  (`spike:metaslab.c:5901-5906`), computed but never exercised
  (`spike:README.md:435-436`).
- Allow a move, with reclaim pinned while the checkpoint exists and a
  writable rewind to it refused once the engine is persistent
  (virtual/backing: `vb:README.md:108`, `130-131`;
  `vb:spa.c:6151-6154`). Its README leaves "the initial product policy"
  to a joint decision.

**11. Acknowledged writes and the keyless ZIL.**

- Redirect never moves a ZIL block (`spike:vdev_raidz.c:8343-8357`).
  Ordinary churn retires them, except those of a dataset whose key is
  not loaded, which it does not handle (#2, open problems).
- Virtual/backing treats every ZIL block as an allocation like any
  other: the pair is bound in a journal before the ZIL acknowledges and
  replayed before claim, log devices included, with no key (21 October;
  `vb:README.md:54`).

Is a journal beside the ZIL acceptable, or does a pool whose keyless
datasets hold a ZIL stay unconverted until their keys are loaded?

**12. Space, the parts open.**

- Before the move: refuse up front unless the end state fits, the new
  extents pack first fit decreasing into the free segments as they
  stand, and what is left covers the slop, the table and the rewrites'
  growth (redirect, `spike:vdev_raidz.c:8573-8752`; a model of the
  allocator, not the allocator); or admit each transaction against a
  standing reserve for the growth still to come, so that a full pool
  still converts (virtual/backing: "793 MiB of weak data inside a
  304 MiB reserve", 21 October).
- During the move: a reservation for the whole migration
  (virtual/backing: the standing reserve; redirect: not built).
- What users see: AVAIL, quotas and reservations during and after a
  move; #5 leaves "public quota/reservation/AVAIL promises" to
  agreement.
- Under the epochs, new-parity writes are still charged at the deflate
  ratio of txg 0; both authors leave it open (#2, 21 October).

**13. The mover and its traversal.** Should the mover use `dsl_scan`'s
sorted, resumable traversal (Q5 of #2)? rdmitry0911 (21 October): good
for sorted I/O and resumption; after holding a txg the mover must wait
for the txg below it to sync, or that txg's frees remove blocks the
walk's root still references; ESTALE and busy are answers the pass
counts, not failures; the gate stays the census. Redirect's mover is
test code that re-checks in syncing context that the old extent is
still allocated and that the bookmark still names the block, and skips
it otherwise (`spike:vdev_raidz.c:8434-8443`). Virtual/backing's
converter walks the MOS, then each dataset with
`traverse_dataset_resume()`, in slices that each start from a fresh root
(`vb:vdev_reloc_convert.c:903-955`). The first evidence asked for (Q6 of
#2), the traversal timed against a scrub, is about the same for
virtual/backing (4 s against 4-5 s, 22 October) and not yet measured for
redirect.

**14. Expansion.** Refuse, at first, to expand a vdev that holds a
mapping and to move on an expanded one (both authors); the epoch series
already refuses to expand an epoch-bearing vdev
(`spike:spa.c:8543-8551`). The combination is first to be defined for
the epochs (openzfs/zfs#19413, question 2). From reading, not tested:
redirect then needs one rule, no move during a reflow, since a reflow
defers no frees (`metaslab.c:4632-4637`); its keys, kept sectors and
copies are offsets a reflow preserves. Virtual/backing's catalog arenas
are "raw per-child regions a reflow would relay out" (21 October), and
its README puts expansion outside the comparison
(`vb:README.md:133-134`).

**15. Interface and observation.** How a move is started, estimated,
watched, paused and cancelled, what `zpool status` shows, and how the
change of tolerance is reported (point 4). A zdb report of DVAs per
parity comes first (section 5). Neither prototype has an interface:
redirect's mover is driven by probes, and virtual/backing has "no
supported administration interface" (`vb:README.md:15-16`).

### Where the earlier questions went

| Raised | Point |
| --- | --- |
| #2, Q1: a per-block redirect, in principle | 1, 2 |
| #2, Q2: a retention-bounded table, estimate and budget | 3 |
| #2, Q3: one feature, gated on a census of zero | 4, 5 |
| #2, Q4: pool-level charge, dataset dsize unchanged | 6, 12 |
| #2, Q5: a mover on `dsl_scan`'s traversal | 13 |
| #2, Q6: traversal timed against a scrub | 13, section 4 |
| 25 October: rewind | 9 |
| 25 October: readers in flight | 8 |
| 25 October: dedup, BRT | 2 |
| 25 October: expansion | 14 |
| rdmitry0911: bootstrap, journal | 7, 11 |
| rdmitry0911: floor, leases | 9, 8 |
| rdmitry0911: checkpoint policy | 10 |
| rdmitry0911: resource budgets, AVAIL promises, product controls | 3, 12, 15 |

## 4. Measurements on one fixture

Each author for their own prototype; both review.

## 5. Landing order

Writes: tigerblue77. Reviews: rdmitry0911.

Draft by tigerblue77, 26 October, from the order proposed in #2, which
rdmitry0911 took into the outline of this document on 21 October, with a
step added for lifetime and space. No on-disk format lands without the
mechanism that completes it: the feature of point 5 of section 3 is
registered only in the last step, or the series merges together.
Every step builds on the parity-epoch series of openzfs/zfs#19262, whose
format lands with the feature of the last step. Steps 1 and 2 do not
depend on the choice between the mechanisms; steps 3 to 6 take their
shape from points 2, 7, 8 and 11.

0. Evidence first: the measurements of section 4 on one fixture and the
   decisions of section 3, presented to the maintainers before any code
   is proposed.
1. Observation: a zdb report of the DVAs on each RAID-Z vdev by parity
   epoch (ahrens on openzfs/zfs#12225: "it would be important to have a
   mechanism to at least observe the amount of old blocks"). Read-only,
   no format, and the reference the census of point 4 is tested against.
2. A refactor of layout selection with no functional change: every
   RAID-Z read, write, repair and resilver check takes its layout from
   one place, so that a later step can give a block a layout other than
   the one its block pointer's birth implies.
3. The mapping and its read paths: format, load and validation; reads
   and repairs at the copy, with the copy's own birth for DTL checks;
   scrub and resilver following the mapping, copies written inside a
   resilver's window included; blocks with several DVAs; frees at the
   last reference; zdb's claims. Entries come only from test code until
   step 6.
4. Lifetime and space: the reader exclusion and the no-deferral rule of
   point 8; the pool-level charge and the move's reservation (points 6
   and 12); the estimate and the budget (point 3).
5. The mover: the rewrite of what a head owns alone, with its BRT and
   nopwrite exceptions (point 2); the mapping of the rest; the traversal
   (point 13); the keyless ZIL (point 11); the checkpoint rule (point
   10). Until step 6 it runs only from test code, so that no supported
   path writes a mapping before the feature exists.
6. The feature, registered last, with the interface of point 15 that
   starts a move: active before the first incompatible state is durable
   and while any durable state needs it (point 5); the census of point
   4, its durable record and the rules that withdraw it, the new
   tolerance and the retirement of the old epoch; the guard on retired
   physical layouts, applied after a block's mapping is validated and
   resolved, so that an unchanged block pointer with a birth in the
   retired epoch is still read, repaired, freed and claimed (point 4);
   `zpool-features.7` and the man page of the command that starts a
   move.

Each step comes with its tests: ZTS for the kernel paths, `ztest` on
RAID-Z, zdb's checks on the pools it leaves, and, from step 3 on, the
fault cases of section 4 and the crash points of requirement 6 of
section 1.
