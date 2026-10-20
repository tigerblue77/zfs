- 2026-10-13: #19376 amended to 38e98a3 (cheap checks before SCL_VDEV in bp_asize_is_current, man -W text, test file S). Integration 6d5590f still carries 51a4c9f's version: rebase onto 38e98a3 at next rebuild.
- 2026-10-14: #19377 now 5c84532 (test holds the volume open). New #19398 (zvol-open-renamed, a5cdbb6, on c27dff0) fixes #19397 (zv_name lookups during rename: ENOENT on open, crash after rollback). Integration branch carries neither yet.
- 2026-10-14: probe 'zfs set readonly=on' right after rename (master): 0/100 writes accepted, same as control. Not reproduced from the CLI; not pursued.
- 2026-10-14 ~21h: #19374 merged by behlendorf as 63c423a. All 6 open PR branches merge cleanly onto it. Pairwise conflicts unchanged (recipes) + new trivial one: #19398 vs #19377 (linux.run, Makefile.am: both add a test). After #19375 merges: raidz_expand_rewrite.ksh uses the default checksum because of #19372; a follow-up can also cover sha256 (nopwrite).
- 2026-10-17: #19376 merged (5d7c42c), #19398 merged (b31984c, closes #19397). Rebased on 073e326, NOT pushed yet
  (awaiting force-push OK): #19367 -> 13f22c6+39b2d87 (-W moved into dmu_rewrite_skip, works on volumes; VALID_FLAGS
  next to the flag defines), #19375 -> f944383 (runfile), #19377 -> cf3bbc4 (lists; ENOENT hold removed from test and
  commit msg). Backups: tags backup/pr19367-7e7a553, backup/pr19375-61961d7, backup/pr19377-5c84532.
  VM (debug, 6.12): all zfs_rewrite tests + raidz_expand_rewrite PASS on both #19367 and #19375 builds;
  zvol_misc_rename_write/open PASS on #19377; rename_write FAILs on master 073e326 with EPERM (5 writes).
- 2026-10-17: new PR branch zfs-rewrite-skip-unchanged (6dbd356, -W extension: bp_rewrite_is_current(), spa_class_select(),
  flag renamed ZFS_REWRITE_SKIP_CURRENT). Local integration branch rewrite-integration-1017 (4b98a98) = 073e326 + race + nopwrite
  + -W ext + volumes + uevent. Recipes: nopwrite x race = replay of old dbuf.c resolution (1101872) via merge-file;
  volumes x -W ext = drop zfs_rewrite_skip/VALID_FLAGS/policy from zfs_vnops.c, dmu_rewrite_skip(db, os, flags, wp, txg)
  calls bp_rewrite_is_current, dmu_rewrite_by_dnode computes wp with dmu_write_policy(dn->dn_objset, dn, 0, 0, &wp)
  when SKIP_CURRENT; volumes x nopwrite = dmu_buf_will_rewrite(db, tx, physical). VM: all 8 zfs_rewrite tests,
  raidz_expand_rewrite, zvol_misc_rename_write PASS; -W on a zvol after compression=zstd moves all 512 blocks, then none.
