# Removing a raidz top-level vdev by raw-segment copy: code facts at c27dff0c3

Source: upstream master `c27dff0c3`, read with `git show c27dff0c3:<path>` from
`$SCRATCH/pr/zfs`.
All line numbers are at that commit.

Markers: **[F]** = read in the code (quoted or cited); **[I]** = my inference from
those facts; **[D]** = derivation (arithmetic on code that is quoted), as solid as
the quoted code. Notation: W = physical width (children), P = nparity,
a = ashift, sector = 2^a bytes. "Parent sector" = raidz top-level offset >> a.

---

## 0. Headline results

1. **[F/D] The raidz parent address space is a plain interleave**: parent sector
   `s` lives on child `s mod W` at child sector `s div W` (W = current physical
   width). Every column of every block, its parity and its skip sectors obey it.
   `vdev_raidz_xlate()` encodes it, `vdev_raidz_io_verify()` asserts it (debug)
   for every write, and raidz expansion's reflow relies on it. So copying
   `[X, X+len)` raw, child by child, onto `[Y, Y+len)` of an identical raidz keeps
   each child index and only shifts child offsets **iff `(Y-X)/2^a ≡ 0 (mod W)`**.
2. **[F] Nothing in the read path can cope today**: a raidz top-level I/O with
   `io_bp == NULL` dereferences NULL (`vdev_raidz.c:2792-2793`); the indirect
   vdev sends NULL-bp child I/Os for every split block; the indirect vdev remaps
   only `io_size` (rounded psize), not the raidz asize footprint, so a raidz
   block whose parity tail crosses a mapping-entry boundary is misclassified as
   non-split. The indirect vdev keeps no geometry (no tsd, no config), and the
   raidz expansion txgs live only in the raidz vdev's config nvlist and tsd, so
   they are lost when the vdev becomes indirect.
3. **[F] The allocator cannot express a congruence**: `metaslab_alloc_dva()` takes
   a psize (inflated to raidz asize by the destination) and has no alignment or
   phase argument; allocators return free-segment starts.
4. **[I] Minimal design constraints**: same physical width W, same ashift, (same
   P if the destination ever interprets the data); per mapping entry
   `Y ≡ X (mod W·2^a)`; entry sizes and split points a multiple of
   `(P+1)·2^a`; every source child readable with an empty DTL; no expansion in
   progress on source or destination; the source geometry (W, P, a, expansion
   txgs, deflate ratio) persisted on the indirect vdev behind a new feature flag;
   reads of raidz-sourced indirect blocks decoded with the **source** geometry
   (map built from the original offset), with column I/Os translated through the
   mapping. raidz1's bit-20 parity swap matters only if the **destination** builds
   the map (direct read or `spa_remap_blkptr()`).

---

## 1. RAIDZ geometry as a function of offset

### 1.1 Classic map: `vdev_raidz_map_alloc()` (`vdev_raidz.c:640-773`) [F]

Used when the block's logical width equals the physical width (`vdev_raidz.c:2871-2873`).

```c
uint64_t b = zio->io_offset >> ashift;          /* 645 parent sector of block */
uint64_t s = zio->io_size >> ashift;            /* 647 */
uint64_t f = b % dcols;                         /* 649 first column's child */
uint64_t o = (b / dcols) << ashift;             /* 651 child byte offset */
uint64_t q = s / (dcols - nparity);             /* 662 */
uint64_t r = s - q * (dcols - nparity);         /* 668 */
uint64_t bc = (r == 0 ? 0 : r + nparity);       /* 671 big columns */
uint64_t tot = s + nparity * (q + (r == 0 ? 0 : 1)); /* 677 */
```

- Columns `c = 0 .. scols-1` (`705-724`): `col = f + c`; if `col >= dcols` then
  `col -= dcols; coff += 1 << ashift` → `rc_devidx = (f+c) mod W`,
  `rc_offset = o` or `o + 1 sector` when wrapped. Sizes: `(q+1)` sectors for
  `c < bc`, `q` otherwise, 0 for `c >= acols` (skip-only columns when `q == 0`,
  `683-690`: `scols = MIN(dcols, roundup(bc, nparity + 1))`).
- Parity = columns `0 .. P-1` (`rr_firstdatacol = nparity`, `697`); data columns
  follow, each a contiguous slice of `io_abd` (`624-629`, `574-603`).
- Skip/padding (`727-728`): `rm_nskip = roundup(tot, nparity + 1) - tot;
  rm_skipstart = bc;`. For writes, skip sectors are written as zeros, wrapping
  onto the next row of the first `nwrapped` parity columns (`516-526`, `563-571`)
  or as a gang ABD tail on data columns (`593-598`); zeroed skip sectors are not
  used in parity (`585-591`). Optional NODATA skip writes: `raidz_start_skip_writes()`
  (`2610-2631`), only when logical == physical width (`2886-2888`).
- **raidz1 swap** (`753-762`):
  ```c
  if (rr->rr_firstdatacol == 1 && (zio->io_offset & (1ULL << 20))) {
      /* swap rr_col[0] and rr_col[1] devidx/offset */
      if (rm->rm_skipstart == 0) rm->rm_skipstart = 1;
  }
  ```
  Condition is `rr_firstdatacol == 1`, i.e. **raidz1 only**; there is nothing
  similar for raidz2/3 (no other offset-dependent permutation in either map
  function). The comment (`730-749`) calls it an on-disk format requirement
  "for all eternity". zdb replicates it (`cmd/zdb/zdb.c:2947-2952`).

### 1.2 Expanded map: `vdev_raidz_map_alloc_expanded()` (`vdev_raidz.c:791-1105`) [F]

Used when `logical_width != vd_physical_width` (`2792-2869`), i.e. for blocks born
before a completed expansion, and for everything while an expansion is in progress.

- `q, r, bc, tot` as above but with `logical_cols` (`808-823`); `rows =
  howmany(tot, logical_cols)` (`826`).
- Per row (`843-867`):
  ```c
  uint64_t b = (offset >> ashift) + row * logical_cols;   /* 843 */
  int row_phys_cols = physical_cols;
  if (b + cols > reflow_offset_synced >> ashift)
      row_phys_cols--;                                    /* 858-860: not yet reflowed */
  uint64_t child_id = b % row_phys_cols;                  /* 865 */
  uint64_t child_offset = (b / row_phys_cols) << ashift;  /* 867 */
  ```
  Columns step `child_id++` with wrap to the next child row (`892-899`). Parity is
  the first P columns of **each row** (`913-918`). Data sector `(row, dc)` comes from
  abd sector `dc*rows + row` (or the short-column variant, `933-938`), so data is
  column-major across rows. Short last row: phantom zero sectors (`920-928`).
- raidz1 swap per row, still decided by the **block** offset (`979-1001`):
  `if (rr->rr_firstdatacol == 1 && rr->rr_cols > 1 && (offset & (1ULL << 20)))`.
- During a reflow, shadow writes to the new location and `use_scratch` handling
  (`945-971`, `2828-2869`) make the geometry depend on reflow progress.

### 1.3 The interleave property [D, backed by F]

Classic map, column `c`, `i`-th sector: child `(f+c) mod W`, child sector
`o + i + wrap`. Its parent sector is `(o+i+wrap)·W + (f+c) mod W = b + c + i·W`.
The union over columns is exactly `[b, b+tot)` (for `r ≠ 0`, `tot = qW + bc`), and
the skip sectors are `[b+tot, b+tot+nskip)`. A "row" of a block is the parent range
`[b+iW, b+(i+1)W)`. Expanded map: row `r` is `[b + r·L, b + r·L + L)`, located by
`s mod W_phys, s div W_phys` after reflow. In both cases **parent sector s ↦
(child s mod W_phys, child sector s div W_phys)**.

Code that states or relies on it [F]:
- `vdev_raidz_xlate()` (`4238-4287`): for child `tgt_col`, parent range
  `[b_start, b_end)` maps to child rows `start_row = ((b_start - tgt_col - 1) / width) + 1`
  .. `end_row` (`4270-4282`), `width = vd_physical_width` (`4263`).
- `vdev_raidz_io_verify()` (`2522-2560`, debug): for each column asserts
  `rc->rc_offset == physical_rs.rs_start` from `vdev_xlate()` of the row's
  parent range.
- Reflow copies parent sectors raw: writes to `vdev_child[(blkid + i) % vdev_children]`
  at `((blkid + i) / vdev_children) << ashift`, reads from
  `vdev_child[(blkid + i) % old_children]` (`raidz_reflow_impl()`, `4640-4678`).
  Blocks keep their DVA offset across expansion; only the logical width chosen at
  read time (by birth txg) changes.

### 1.4 asize [F]

`vdev_raidz_psize_to_asize_width()` (`2376-2388`):
`asize = ceil(psize/sector); asize += P * ceil(asize/(cols-P)); return roundup(asize, P+1) << a`.
`vdev_raidz_alloc_factor()` (`5682-5686`) = `(P+1) << a`; allocators round sizes to it
(`metaslab.c:1797`, `1965`, `2019`, `2095`) and assert it when `size == max_size`
(`ASSERT0(cand_size % mult)`, `metaslab.c:1799`, `1967`, `2021`, `2097`).

### 1.5 Congruence conditions

Copy model: for each child j, the source child sectors holding parent sectors
`{s ∈ [X, X+len) : s ≡ j (mod W)}` (contiguous on the child, cf. `vdev_raidz_xlate`)
are written to the same child index j of the destination.

**[D] Well-definedness of "same child index" copying.** The destination allocation
`[Y, Y+len)` owns on child j the sectors `{s' ∈ [Y, Y+len) : s' ≡ j}`. Counts and
positions match the source's for every j iff `Y ≡ X (mod W)` (in sectors), or
`len ≡ 0 (mod W)`. Otherwise some child gets one sector more than its slice and the
write spills into a neighbour's space. With `Y ≡ X (mod W)` every child is shifted
by the same `Δ = (Y-X)/W` child sectors, and parent sector `s` maps to `s + (Y-X)`.

**(a) Destination builds the map** (I/O reaches `vdev_raidz_io_start()` on the
destination with offset `B' = B + (Y-X)` for a block at source offset `B`):
the map is identical to the source's iff all of the following hold. [D] unless noted.
1. Same W_phys, P (`vdrz->vd_nparity` used at `2866-2873`), and ashift
   (`tvd->vdev_ashift`).
2. `(Y - X) >> a ≡ 0 (mod W_phys)`, `X, Y` sector aligned. Then `f' = f`,
   `o' = o + Δ`, identical column sizes (they depend only on size and width).
3. raidz1 only: bit 20 of every block start must be preserved:
   `((X+t) >> 20) & 1 == ((Y+t) >> 20) & 1` for every block start `X+t` in the
   entry. Block starts are unknown to a raw copy, so this must hold for every
   sector t. For an entry crossing a 1 MiB boundary of the source that forces
   `Y ≡ X (mod 2^21)`; combined with (2): `Y ≡ X (mod lcm(W·2^a, 2^21))`
   (e.g. W=6, a=12: lcm(24 KiB, 2 MiB) = 6 MiB). An entry that crosses no 1 MiB
   boundary only needs `bit20(X) == bit20(Y)` and the same "no crossing" on the
   destination.
4. Same logical width per block: `vdev_raidz_get_logical_width(dst, birth) ==
   L_src(birth)` for the txg the destination will use (§4), and no reflow in
   progress on the destination.
5. The I/O must cover the whole block: the map is computed from `io_size`; a
   partial range is decoded as if it were a block of that size (wrong parity
   columns, wrong data order) [F: map code depends only on offset/size].

**(b) Map built from the ORIGINAL source offset, child I/Os translated**:
1. Bit 20 is irrelevant (decided by the source offset, as when written) and the
   destination's logical width / expansion history is irrelevant [D].
2. For a block entirely inside one entry, any per-child translation that is a
   bijection on child indices works; with same-index copying this is the
   condition of "well-definedness" above (`Y ≡ X (mod W)`) [D].
3. For a block split across entries, each parity **row** (W consecutive parent
   sectors, or L for expanded rows) must keep its sectors on distinct disks. With
   `Y_e ≡ X_e (mod W)` for every entry e, child index is preserved for every
   sector, so a row straddling an entry boundary still lands on distinct children
   (same vdev: different child indices; different vdevs: different disks). [D]
   Without a common residue, two sectors of the straddling row can land on the
   same child of the same destination vdev → that row loses one level of
   redundancy. Row boundaries depend on the unknown block start (`b + iW`), so a
   raw copy cannot cut entries at row boundaries. [D]
4. So the minimal per-entry condition is `Y ≡ X (mod W·2^a)` (bytes), nothing
   for bit 20, nothing on the destination's history. [D]

**[I] Caveat (destination expanded after removal).** Reflow preserves parent
sectors but changes the interleave to W+1. Whole rows stay on distinct children
(consecutive parent sectors), but rows split across two entries placed on the
same destination vdev are only guaranteed distinct for the residue mod W, not mod
W+1. Either forbid expanding a raidz that is a destination of a raidz-sourced
indirect mapping, or accept reduced redundancy on those rows.

---

## 2. How an indirect vdev serves reads today (`module/zfs/vdev_indirect.c`)

- [F] Entry point: the root vdev uses mirror ops over DVAs; it sets
  `mc_offset = DVA_GET_OFFSET(&dva[c])` (`vdev_mirror.c:320-321`) and issues
  `zio_vdev_child_io(zio, zio->io_bp, mc->mc_vd, mc->mc_offset, zio->io_abd,
  zio->io_size, ...)` (`vdev_mirror.c:687-690`). `io_size` is the psize given to
  `zio_read()` (`zio.c:1344-1351`), rounded to the top-level ashift in
  `zio_vdev_io_start()` (`zio.c:5123-5135`). It is **not** the raidz asize.
- [F] `vdev_indirect_io_start()` (`1295-1382`): `vdev_indirect_remap(zio->io_vd,
  zio->io_offset, zio->io_size, vdev_indirect_gather_splits, zio)` (`1316-1317`).
  Writes only for self-heal/resilver/induce-damage (`1306-1314`).
- [F] `vdev_indirect_remap()` (`1056-1175`): copies the overlapping mapping entries
  under `vdev_indirect_rwlock` (`1092-1100`), walks them, calls
  `func(rs_split_offset, dst_v, dst_offset + inner_offset, inner_size)`
  (`1160-1162`); if `dst_v` is itself indirect, pushes a segment and recurses
  (`1134-1139`). Debug flag `ZFS_DEBUG_INDIRECT_REMAP` halves pieces (`1141-1158`).
- [F] `vdev_indirect_gather_splits()` (`1194-1236`): one `indirect_split_t` per
  concrete piece: `is_vdev` = **top-level** destination, `is_target_offset`,
  `is_size`, `is_split_offset`. Children considered: mirror children only
  (`1205-1207`, `1226-1230`); any other type gets one child = the top-level vdev
  itself (`1231-1233`).
- [F] Non-split (`first->is_size == zio->io_size`, `1321-1342`):
  `zio_vdev_child_io(zio, zio->io_bp, first->is_vdev, first->is_target_offset,
  ..., zio->io_size, ...)` → destination top-level `vdev_op_io_start`, with the
  bp, so the child verifies the checksum and can use its own redundancy.
- [F] Split, normal read (`1367-1376`): per piece
  `zio_vdev_child_io(zio, NULL, is->is_vdev, is->is_target_offset,
  abd_get_offset_size(zio->io_abd, is->is_split_offset, is->is_size), is->is_size, ...)`
  → destination **top-level** vdev ops, **no bp**. Checksum verified in
  `vdev_indirect_io_done()` (`1832-1850`); on failure `vdev_indirect_read_all()`
  and `zio_vdev_io_redone()` (`1857-1859`).
- [F] Split, scrub/resilver or after a checksum failure: `vdev_indirect_read_all()`
  (`1257-1292`) reads every `ic_vdev` (mirror leaf or the top-level), NULL bp,
  marks `ESTALE` from the DTL using `zio->io_txg` (`1277-1279`).
- [F] Reconstruction (`vdev_indirect_reconstruct_io_done()`, `1720-1807`):
  dedups identical copies per split (`1748-1785`), enumerates all combinations if
  `<= zfs_reconstruct_indirect_combinations_max` (4096, `215`), else random
  (`1787-1790`); on success repairs bad copies with NULL-bp writes of `is_size` to
  `ic_vdev` (`vdev_indirect_repair()`, `1422-1458`, write at `1440-1445`).
- [F] `vdev_indirect_ops` (`1862-1886`): `op_init/fini = NULL`,
  `psize_to_asize = vdev_default_asize`, `xlate = NULL`, `config_generate = NULL`,
  `nparity = NULL`, `ndisks = NULL`.

**If the destination were a raidz top-level vdev** [F unless noted]:
- Split piece or `read_all`/repair: `vdev_raidz_io_start()` evaluates
  `BP_GET_PHYSICAL_BIRTH(zio->io_bp)` unconditionally (`vdev_raidz.c:2792-2793`);
  `io_bp` is NULL because `zio_create()` only sets it when `bp != NULL`
  (`zio.c:997-1005`, after `memset(zio, 0, ...)` at `zio.c:976-977`) → NULL
  dereference (and `BP_GET_DEDUP(zio->io_bp)` at `2795`, debug
  `vdev_raidz_io_verify` at `2529-2530`, `ASSERT(zio->io_bp != NULL)` in
  `vdev_raidz_io_done()` at `4014`).
- [I] Even with a guard, the map of a partial piece treats its first P columns as
  parity and returns a re-ordered mix of data and parity; a repair write would
  generate new parity over the piece and write `tot > is_size` sectors, past the
  piece (overwriting neighbours).
- Non-split: the destination builds its own map at `Y+t` using **its** expansion
  history and the original BP birth (§4); correct only under §1.5(a).
- Footprint bug [F + D]: the indirect vdev remaps `io_size` (rounded psize), but a
  raidz block occupies `[B, B + asize)` (data + parity + skip). A block whose tail
  crosses an entry boundary is classified non-split (`1321`) and handed whole to
  the first piece's destination, which then reads `tot` sectors past the entry's
  end, i.e. unrelated destination space. Any raidz-aware indirect read must size
  the remap with the **source** `psize_to_asize(psize, birth)`.
- Dedup check: `vdev_raidz_io_exceeds_dva()` is reached when logical ≠ physical
  width and the BP is dedup (`2794-2826`); it matches DVAs by
  `DVA_GET_VDEV(dva) != tvd->vdev_id || DVA_GET_OFFSET(dva) != zio->io_offset`
  (`2433-2435`), which a remapped I/O never satisfies → `owned_asize == 0` →
  `ASSERT3U(*owned_asizep, !=, 0)` (`2451`) / EIO. Its comment states the current
  invariant: "Indirect vdevs cannot coexist with a RAIDZ top (device removal and
  vdev-add both refuse), so remapped I/O never reaches this check." (`2418-2423`).
- **No existing hook keeps the source's geometry** [F]: no tsd, no
  `config_generate` (`vdev_label.c:523-524` calls it only if non-NULL), and
  `vdev_indirect_config_t` holds only `vic_mapping_object`, `vic_births_object`,
  `vic_prev_indirect_vdev` (`include/sys/vdev_impl.h:157-187`). Mapping entries are
  `{vimep_src (63-bit offset + mark), dva_t vimep_dst}`
  (`include/sys/vdev_indirect_mapping.h:29-43`).

---

## 3. How removal copies data today (`module/zfs/vdev_removal.c`)

- [F] Thread: `spa_vdev_remove_thread()` (`1622-1849`) loads each metaslab's
  allocated segments into `svr_allocd_segs` (`1686-1709`) and calls
  `spa_vdev_copy_impl()` per txg (`1778`).
- [F] `spa_vdev_copy_impl()` (`1469-1589`) gathers segments into one chunk, joining
  free gaps up to `vdev_removal_max_span` = 32 KiB (`160`, `1501-1507`), capped by
  `*max_alloc` (`1497-1499`, `1508-1515`), then calls `spa_vdev_copy_segment()`
  with `thismax = SPA_MAXBLOCKSIZE` (`1550-1553`). On `ENOSPC`:
  `thismax = P2ROUNDUP(attempted / 2, 1 << spa->spa_max_ashift)`,
  `*max_alloc = attempted - (1 << spa->spa_max_ashift)` (`1555-1576`, with
  `ASSERT3U(spa->spa_max_ashift, ==, spa->spa_min_ashift)` at `1566`).
- [F] `spa_remove_max_segment()` = `P2ROUNDUP(zfs_remove_max_segment, 1 <<
  spa_max_ashift)` (`1600-1603`); `zfs_remove_max_segment = SPA_MAXBLOCKSIZE`
  (`134`).
- [F] `spa_vdev_copy_segment()` (`1147-1279`): if the span exceeds `maxalloc`, ends
  the allocation at the last segment end before `start + maxalloc`, or, when the
  first segment is larger, **splits it at exactly `maxalloc`** (`1163-1186`,
  `size = maxalloc` at `1184`). This is where blocks get split, at arbitrary sector
  offsets. Allocation:
  `metaslab_alloc_dva(spa, mc, size, &dst, 0, NULL, txg, 0, zal, 0)` from the
  source's class, retried on the normal class (`1193-1201`). Then
  `VERIFY3U(DVA_GET_ASIZE(&dst), ==, size)` (`1239`), entry
  `{src = start, dst}` (`1241-1243`), obsolete count = free gaps (`1210-1229`,
  `1244-1247`).
- [F] I/O level (`spa_vdev_copy_one_child()`, `1084-1140`): one write per
  destination "child" (mirror child leaf, else the destination top-level vdev,
  `1262-1271`), each fed by one read. Source: if source is a mirror **and** the
  destination is a mirror, read mirror child `(dest_id + i) % children`, the first
  readable one (`1104-1116`); otherwise read the **top-level** source vdev
  (`1117-1119`). Both I/Os have `bp = NULL` (`1129-1139`); no checksum is verified
  ("Since we aren't verifying checksums", `1043-1049`). Unwritable destination
  children are skipped silently (`1094-1095`).
- [F] Mirrors as source: all sides copied only when the destination is also a
  mirror (side i → side i, wrapping); mirror → single disk reads one side through
  the mirror ops.
- [F] With a raidz source (non-mirror branch) the read hits `vdev_raidz_io_start()`
  with NULL bp (§2); with a raidz destination the write does too. [I] Without the
  crash, the destination would compute parity for a pseudo-block of `size` bytes
  and write `tot > size` sectors, overrunning the allocation.
- **Destination choice and alignment** [F]:
  - `metaslab_alloc_dva()` (`metaslab.c:5538-5544`) has no alignment/phase
    argument; flags are `METASLAB_ZIL|GANG_HEADER|GANG_CHILD|ASYNC_ALLOC`
    (`include/sys/metaslab.h:68-71`); `hintdva` only picks the first vdev
    (`metaslab.c:5467-5470`); otherwise the class rotor (`5475-5515`). The removal
    comment notes the lack of a way to request a vdev type (`vdev_removal.c:94-97`).
  - psize is converted with the destination's op:
    `asize = vdev_psize_to_asize_txg(vd, psize, txg)` (`metaslab.c:5491`) → for a
    raidz destination the DVA is larger than `size` and the `VERIFY` at
    `vdev_removal.c:1239` fires. An asize-based entry point does not exist
    (`metaslab_group_alloc()` takes asize but is static, `5062-5066`).
  - Allocators (`metaslab_df_alloc` `1906-1976`, `metaslab_cf_alloc` `1988-2028`,
    `metaslab_ndf_alloc` `2046-2104`, helper `metaslab_block_picker` `1774-1813`)
    return the **start of a free segment** (or the cursor), e.g.
    `uint64_t offset = zfs_rs_get_start(rs, rt); ... return (offset);`
    (`1792-1804`). df's `align = max_size & -max_size` only picks a cursor bucket
    and "does not guarantee" alignment (`1909-1917`). The only enforced offset
    alignment is `VERIFY0(P2PHASE(start, 1ULL << vd->vdev_ashift))` (`4925`).
  - [I] A congruence `Y ≡ X (mod W·2^a)` (W not a power of two in general, so no
    `P2*` macros) would need either a phase-aware search inside each `msop_alloc`
    (find first `off ≥ seg_start`, `off ≡ r (mod M)`, `off + size ≤ seg_end`) plus
    an asize-based, vdev-restricted `metaslab_alloc_dva` variant, or an
    over-allocation of `size + M - sector` whose head/tail is freed back (creates
    sub-`(P+1)` fragments that no raidz allocation can reuse).

---

## 4. RAIDZ expansion history and remapped blocks

- [F] `vdev_raidz_get_logical_width_locked()` (`2275-2296`): exact or nearest
  earlier `re_txg` in `vd_expand_txgs`, else `vd_original_width`. "Blocks with
  physical birth time of re_txg or later have the specified logical width"
  (`include/sys/vdev_raidz_impl.h:150-158`). `re_txg = tx_txg +
  TXG_CONCURRENT_STATES` at completion (`raidz_reflow_complete_sync()`,
  `4345-4350`).
- [F] Read: `vdev_raidz_io_start()` uses
  `vdev_raidz_get_logical_width(vdrz, BP_GET_PHYSICAL_BIRTH(zio->io_bp))` of
  **the vdev doing the I/O** (`2792-2793`).
- [F] Storage: the txg list is in the raidz vdev's **config nvlist**
  (`ZPOOL_CONFIG_RAIDZ_EXPAND_TXGS`, read in `vdev_raidz_init()` `5566-5583`,
  written by `vdev_raidz_config_generate()` `5644-5661`) and in the tsd AVL. The
  top ZAP holds only state / start / end / bytes copied
  (`VDEV_TOP_ZAP_RAIDZ_EXPAND_*`, `vdev_raidz_load()` `5416-5440`,
  `raidz_reflow_sync()` `4329-4331`).
- [F] Survival: `vdev_remove_replace_with_indirect()` (`vdev_removal.c:1338-1373`)
  does `vdev_add_parent(vd, &vdev_indirect_ops)`; `vdev_add_parent()`
  (`vdev.c:1437-1468`) allocates a fresh vdev (no tsd since `op_init == NULL`) and
  `vdev_top_transfer()` (`vdev.c:1309-...`) moves the top ZAP (`1321`), indirect
  config (`1378-1380`) and deflate ratio (`1411-1412`). The raidz child holding
  `vdev_raidz_t` is detached and freed (`vdev_raidz_fini()` `5596-5612`). The
  indirect vdev's config has no `op_config_generate` → **expansion txgs, width and
  nparity are lost**; the ZAP keys survive but nothing reads them
  (`vdev_raidz_load` runs only for raidz ops, `vdev.c:3982-3985`).
- Which history and txg is used for a block that moved [F]:
  - Read through the indirect vdev, non-split: the **destination's** history with
    the **original** physical birth (bp passed at `vdev_indirect.c:1338`).
  - Split: no bp → crash (§2).
  - After `spa_remap_blkptr()` (`metaslab.c:5712-5784`; refuses dedup, gang,
    embedded, BRT, split, `5726-5749`, `5643-5645`): DVA moved to the destination
    and `BP_SET_PHYSICAL_BIRTH(bp, physical_birth)` with
    `physical_birth = vdev_indirect_births_physbirth(...)`, the txg of the copy
    (`5674-5693`) → the **destination's** history at the **copy txg**.
  - The source's history is never consulted after completion.
- Implications [I]:
  - Source expanded before removal: blocks born before its expansion have
    `L < W_src`. Read via the destination (direct or remapped) they get
    `L_dst(birth) = W` (never-expanded destination) or `L_dst(copy txg) = W` →
    wrong map. Only a source-geometry decoder (§1.5 b) with the persisted txg list
    can read them, and remap must be refused for them.
  - Destination expanded **before** the removal: `L_dst(original birth)` is the
    destination's older, narrower width for births before its expansion → wrong
    map on the non-remapped path; remapped BPs (copy txg) are fine. Safe only if
    the destination's last expansion `re_txg` ≤ every source birth (bounded below
    by the source's `vdev_crtxg`).
  - Destination expanded **after** the removal: whole blocks stay consistent
    (reflow is parent-linear, and both the original birth and the copy txg predate
    the new `re_txg`), but see the split-row caveat in §1.5.
  - Expansion in progress on source or destination: geometry depends on
    `reflow_offset_synced` and the range lock (`2844-2869`); must be refused.
    `spa_vdev_attach()` already refuses while a removal runs (`spa.c:8521-8524`);
    nothing refuses a removal while an expansion runs, because raidz is refused
    earlier.

---

## 5. Everything in the removal path that assumes a non-raidz source or destination

| Where | What it assumes | Change |
| --- | --- | --- |
| `vdev_removal.c:2327-2330` `spa_vdev_remove_top_check` | `if (vdev_get_nparity(cvd) != 0) return EINVAL` for **every** concrete top-level (all classes, including the one removed) | Allow raidz source/destinations of identical W, P, a; refuse draid, mixed geometry, expansion in progress |
| `vdev_removal.c:2281-2289` | Top-level `DTL_MISSING/OUTAGE` empty and `vdev_readable(vd)`; for raidz the top DTL needs P+1 missing children (`vdev.c:3541-3543`) | [I] Also require every source child readable, leaf, and child DTLs empty (reflow precedent: `vdev_raidz.c:4484`, `4530-4538`) |
| `vdev_removal.c:2294-2305`, `2319-2326` | One ashift for the class | Unchanged (needed) |
| `vdev_removal.c:448` `vdev_remove_initiate_sync`, `1323` `vdev_remove_enlist_zaps` | `ASSERT0(vdev_get_nparity(vd))` | Drop; persist source geometry here |
| `vdev_removal.c:1084-1140` `spa_vdev_copy_one_child` | Top-level read/write with NULL bp; mirror-only per-child pairing | Per-child raw copy (child j → child j, `vdev_xlate` ranges) |
| `vdev_removal.c:1147-1279` `spa_vdev_copy_segment` | psize allocation, `VERIFY(asize == size)` `1239`, split at `maxalloc` `1184`, any vdev in class | asize + congruent + same-geometry allocation; split points multiple of `(P+1)·2^a` |
| `vdev_removal.c:1555-1576`, `1600-1603` | Halving / max segment rounded to `2^a` only | Round to `(P+1)·2^a` (or `lcm(W,P+1)·2^a`) |
| `metaslab.c:5413-5544` | No asize/alignment/vdev filter | New allocation entry point and phase-aware `msop_alloc` |
| `vdev_indirect.c:1295-1382` | Remaps `io_size`; NULL-bp pieces to top-level | Source-geometry raidz decode with column translation; footprint = source asize |
| `vdev_indirect.c:1194-1236`, `1257-1292`, `1422-1458`, `1720-1807` | Copies = mirror children only; repair = rewrite whole piece | Column-level DTL, reconstruction and repair via the raidz code |
| `vdev_indirect.c:1862-1886` | No init/fini/config/nparity | tsd + `config_generate` for persisted geometry (feature-gated) |
| `vdev.c:2120-2127`, `3980`; `spa_misc.c:2378-2393` | Deflate ratio recomputed at load from `psize_to_asize`; indirect gives 512 (`vdev_default_asize`, `vdev.c:358-369`) while the raidz's ratio is lower; `dva_get_dsize_sync` uses it | [I] Persist/compute the source ratio, else dataset space accounting of old blocks changes after the next import |
| `vdev_raidz.c:2792-2826`, `2418-2423`, `2529-2530`, `4014` | Raidz I/O always has a bp; remapped I/O never reaches the dedup check | Handle NULL bp or never send it; DVA matching for remapped I/O |
| `metaslab.c:5637-5697`, `5712-5784` `spa_remap_blkptr` | Destination reads the remapped DVA with its own geometry and the copy-txg birth | [I] Refuse remap unless §1.5(a) holds for the entry (congruence, bit 20 for raidz1, `L_src(birth) == L_dst(copy txg)`); simplest: refuse for raidz-sourced indirect vdevs |
| `spa.c:8277-8300` `spa_vdev_add` | No raidz/draid top-level may be added while indirect vdevs exist | Relax for matching geometry |
| `dsl_scan.c:3916-3925` | Indirect DVAs always need resilver I/O | Unchanged; the I/O path must handle it (§2) |
| `vdev_indirect.c:589-635` condensing; `vdev_indirect.c:321-353`, `vdev_removal.c:685-862` obsolete/frees | Pure raw-offset arithmetic on DVA asize | [I] No change; frees of partial pieces on a raidz destination can leave sub-`(P+1)` fragments if pieces are not on the `(P+1)` grid |
| `vdev_removal.c:1905-2040` cancel | Frees mapped ranges raw | [I] No change |
| `vdev_removal.c:1043-1049` | No checksum verification during copy | [I] Inherent: raw copy has no block boundaries. Silent child damage is copied as-is (as for mirrors) and remains repairable by parity |
| `cmd/zdb/zdb.c:2920-2952`, `3135-3148` | File layout assumes `vdev_child[0]` is raidz and DVA offsets on it | Translate through the indirect mapping; leak checking via `vdev_op_remap` (`zdb.c:7065`) is raw and fine |
| `lib/libzfs/libzfs_pool.c:4462-4467`, `man/man8/zpool-remove.8:51`, `tests/.../removal/remove_raidz.ksh` | User-facing "not be raidz" | Update message, man page, tests; `ztest` removal exercise |
| `module/zcommon/zfeature_common.c:586-` | `device_removal` feature | New feature: older code would crash on NULL-bp raidz I/O of split blocks |

Note: the requested `spa_vdev_remove_allocated_space` does not exist at this
commit; `sr_to_copy` is summed from `metaslab_allocated_space()` in
`vdev_remove_initiate_sync()` (`vdev_removal.c:489-507`), raw asize, so it needs
no change.

---

## 6. Feasibility summary

### Established constraints (from code)
- **[F/D]** Same physical width W, same ashift a. Same P required whenever the
  destination decodes the data itself (direct read, remap); with a source-geometry
  decoder P only matters for the destination's own accounting.
- **[D]** Per mapping entry: `Y ≡ X (mod W·2^a)`. This alone keeps every column on
  its child index, makes per-child copies equal-length and contiguous
  (`vdev_raidz_xlate` ranges), and keeps every parity row of a split block on
  distinct disks.
- **[D]** raidz1 only, and only if the destination builds the map: bit 20 of every
  block start preserved, in practice `Y ≡ X (mod lcm(W·2^a, 2^21))`.
- **[F]** Destination allocation and split sizes multiple of `(P+1)·2^a`
  (allocator asserts).
- **[F/I]** All source children readable leaves with empty DTLs: a missing child
  cannot be regenerated without knowing block (row) boundaries.
- **[F]** No reflow in progress on source or destination.
- **[F]** Source geometry and expansion history must be persisted on the indirect
  vdev; today they are lost.

### Work estimate [I]
| Area | Files (size at c27dff0c3) | Rough new/changed lines |
| --- | --- | --- |
| Feature flag, persisted geometry on indirect vdev (nparity, W, a, expansion txgs, deflate ratio) | `vdev_indirect.c` (1922), `vdev.c` (7328), `vdev_label.c` (2272), `zfeature_common.[ch]`, `zpool-features.7` | 200-350 |
| Congruent, asize-based, geometry-filtered allocation | `metaslab.c` (6531), `metaslab.h` | 150-300 |
| Per-child raw copy, checks, split rounding | `vdev_removal.c` (2624), `spa.c` | 250-450 |
| Raidz-aware indirect read/scrub/resilver/self-heal (source-geometry map, column translation across entries, footprint by source asize) | `vdev_raidz.c` (5733), `vdev_indirect.c` | 800-1500 |
| Remap guard, dedup-check fix | `metaslab.c`, `vdev_raidz.c` | 30-80 |
| zdb, libzfs, man pages | `zdb.c` (10964), `libzfs_pool.c`, `zpool-remove.8` | 100-250 |
| Tests: ztest, ZTS removal with raidz, split blocks, damage, expansion interplay | `cmd/ztest.c`, `tests/zfs-tests/tests/functional/removal/` (36 files) | 400-800 |

Total in the order of 2,000-3,500 lines, most of the risk in `vdev_raidz.c`.

### Hardest open problem [I]
The read side, not the copy. Every raidz call site dispatches columns as
`vd->vdev_child[rc->rc_devidx]` of `zio->io_vd` (e.g. `vdev_raidz.c:2572`, `2619`,
`2646`, `2700`, `2727`, `3343`, `3831`, `3960`, `3986`). A raidz-sourced indirect
vdev must decode with the source geometry, but each column of a split block can
span several mapping entries, so one column may land on the same child index of
**different** destination vdevs at different shifts. The raidz code would need
columns made of several child I/Os, with per-piece DTL/ESTALE handling, errors
merged into `rc_error`, and repair writes split the same way, plus the
`io_size`-vs-asize footprint fix and the deflate-ratio persistence. Next hardest:
a phase-aware allocator (non-power-of-two modulus, 6 MiB for raidz1 W=6 a=12 if the
destination ever decodes), and deciding whether a destination raidz may later be
expanded (split rows lose distinct-disk placement modulo W+1).
