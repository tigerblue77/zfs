/*
 * A vdev with allocating=off that grows must stay accounted once in
 * spa_nonallocating_dspace.
 */
static void
acct(uint64_t id, uint64_t *na, uint64_t *exp, uint64_t *ds, uint64_t *msc)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);
	vdev_t *vd = spa->spa_root_vdev->vdev_child[id];
	uint64_t raw = metaslab_group_get_space(vd->vdev_mg);
	*exp = spa_deflate(spa) ? vdev_deflated_space(vd, raw) : raw;
	*na = spa->spa_nonallocating_dspace;
	*ds = spa_get_dspace(spa);
	*msc = vd->vdev_ms_count;
	spa_config_exit(spa, SCL_ALL, FTAG);
	spa_close(spa, FTAG);
}

static int
do_alloc(uint64_t guid)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_alloc(spa, guid);
	spa_close(spa, FTAG);
	return (e);
}

static void
noalloc_grown_checks(uint64_t na0, uint64_t ds0, uint64_t ms0)
{
	uint64_t na, exp, ds, ms;

	acct(1, &na, &exp, &ds, &ms);
	(void) printf("after growth: nonalloc %llu expected %llu dspace %llu "
	    "ms %llu\n", (u_longlong_t)na, (u_longlong_t)exp,
	    (u_longlong_t)ds, (u_longlong_t)ms);
	chk(ms > ms0, "the vdev grew (%llu -> %llu metaslabs)",
	    (u_longlong_t)ms0, (u_longlong_t)ms);
	chk(na == exp, "nonallocating space counts the vdev once "
	    "(%llu, expected %llu, was %llu)", (u_longlong_t)na,
	    (u_longlong_t)exp, (u_longlong_t)na0);
	chk(ds == ds0, "usable pool space unchanged (%llu -> %llu)",
	    (u_longlong_t)ds0, (u_longlong_t)ds);
	chk(do_alloc(top_guid(1)) == 0, "allocating=on");
	acct(1, &na, &exp, &ds, &ms);
	chk(na == 0, "nonallocating space back to 0 (%llu)", (u_longlong_t)na);
	(void) printf("after allocating=on: dspace %llu\n", (u_longlong_t)ds);
	chk(reimport() == 0, "export + import");
	uint64_t ds_imp;
	acct(1, &na, &exp, &ds_imp, &ms);
	chk(ds_imp == ds, "same usable space after import (%llu vs %llu)",
	    (u_longlong_t)ds_imp, (u_longlong_t)ds);
	chk(scrub_clean(), "scrub clean");
}

static void
sc_noalloc_expand_raidz(void)
{
	spa_t *spa;
	nvlist_t *tops[1];
	nvlist_t *rt;
	uint64_t na0, exp0, ds0, ms0;

	mkpool_top(mk_top("mirror", 2, 0));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "allocating=off on the raidz1");
	sync_n(2);
	acct(1, &na0, &exp0, &ds0, &ms0);
	chk(na0 == exp0, "nonallocating space before (%llu)",
	    (u_longlong_t)na0);
	tops[0] = mk_file(9);
	rt = mk_root(tops, 1);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_attach(spa, top_guid(1), rt, B_FALSE, B_FALSE);
	fnvlist_free(rt);
	chk(e == 0, "expansion of the non-allocating raidz started (%d)", e);
	for (int i = 0; i < 1200 && (spa->spa_raidz_expand != NULL ||
	    !spa_feature_is_active(spa, SPA_FEATURE_RAIDZ_EXPANSION)); i++)
		(void) usleep(100000);
	chk(spa->spa_raidz_expand == NULL, "expansion finished");
	spa_close(spa, FTAG);
	sync_n(3);
	noalloc_grown_checks(na0, ds0, ms0);
	leave_for_zdb();
}

static void
sc_noalloc_online_expand(void)
{
	spa_t *spa;
	uint64_t na0, exp0, ds0, ms0, na, exp, ds, ms;
	char path[1100];

	mkpool_top(mk_top("mirror", 2, 0));
	sync_n(2);
	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile);
	chk(add_top(mk_top(VDEV_TYPE_FILE, 1, 0)) == 0, "add of a disk");
	chk(do_noalloc(top_guid(1)) == 0, "allocating=off on the disk");
	sync_n(2);
	acct(1, &na0, &exp0, &ds0, &ms0);
	chk(na0 == exp0, "nonallocating space before (%llu)",
	    (u_longlong_t)na0);
	VERIFY0(truncate(path, 2 * fsize));
	VERIFY0(spa_open(poolname, &spa, FTAG));
	vdev_state_t st;
	int e = vdev_online(spa, top_guid(1), ZFS_ONLINE_EXPAND, &st);
	chk(e == 0, "zpool online -e (%d)", e);
	spa_close(spa, FTAG);
	for (int i = 0; i < 100; i++) {
		sync_n(1);
		acct(1, &na, &exp, &ds, &ms);
		if (ms > ms0)
			break;
		(void) usleep(100000);
	}
	sync_n(2);
	noalloc_grown_checks(na0, ds0, ms0);
	leave_for_zdb();
}

