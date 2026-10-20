/*
 * Can this vdev be removed by turning it into a hole?  Any top-level vdev
 * that has no allocated space can, whatever its type and whatever else is in
 * the pool: nothing has to be copied.  Log vdevs have their own path.
 */
static boolean_t
vdev_remove_empty_candidate(vdev_t *vd)
{
	return (vd == vd->vdev_top && vdev_is_concrete(vd) &&
	    !vd->vdev_islog && vd->vdev_stat.vs_alloc == 0);
}

/*
 * Is one of the distributed spares of this draid vdev in use?
 */
static boolean_t
vdev_remove_empty_draid_spare_active(vdev_t *vd)
{
	spa_aux_vdev_t *sav = &vd->vdev_spa->spa_spares;

	for (int i = 0; i < sav->sav_count; i++) {
		vdev_t *svd = sav->sav_vdevs[i];

		if (svd->vdev_ops == &vdev_draid_spare_ops &&
		    vdev_draid_spare_get_parent(svd) == vd &&
		    vdev_lookup_by_guid(vd->vdev_spa->spa_root_vdev,
		    svd->vdev_guid) != NULL)
			return (B_TRUE);
	}

	return (B_FALSE);
}

/*
 * The checks of spa_vdev_remove_empty(), done again once the vdev no longer
 * allocates.  ENOTEMPTY means that the vdev holds data, so it has to be
 * evacuated instead.
 */
static int
spa_vdev_remove_empty_check(vdev_t *vd)
{
	spa_t *spa = vd->vdev_spa;

	if (spa_version(spa) < SPA_VERSION_HOLES)
		return (SET_ERROR(ENOTSUP));

	if (vd->vdev_removing)
		return (SET_ERROR(EALREADY));

	/* See spa_vdev_remove_top_check(). */
	if (vd->vdev_mg == NULL)
		return (SET_ERROR(ZFS_ERR_VDEV_NOTSUP));

	if (vd->vdev_stat.vs_alloc != 0)
		return (SET_ERROR(ENOTEMPTY));

	/*
	 * The vdev's space must not be needed: the pool must not be left
	 * with less space than is allocated.
	 */
	if (vd->vdev_mg->mg_class == spa_normal_class(spa) &&
	    !vd->vdev_noalloc) {
		uint64_t available = dsl_dir_space_available(
		    spa->spa_dsl_pool->dp_root_dir, NULL, 0, B_TRUE);
		if (available < vd->vdev_stat.vs_dspace)
			return (SET_ERROR(ENOSPC));
	}

	if (spa->spa_removing_phys.sr_state == DSS_SCANNING)
		return (SET_ERROR(EBUSY));

	if (!vdev_dtl_empty(vd, DTL_MISSING) ||
	    !vdev_dtl_empty(vd, DTL_OUTAGE))
		return (SET_ERROR(EBUSY));

	if (!vdev_readable(vd))
		return (SET_ERROR(EIO));

	/*
	 * Making the hole reopens the root vdev, which a running raidz
	 * expansion does not tolerate.
	 */
	if (spa->spa_raidz_expand != NULL)
		return (SET_ERROR(ZFS_ERR_RAIDZ_EXPAND_IN_PROGRESS));

	/*
	 * A distributed spare that replaces a child cannot outlive its
	 * draid vdev.
	 */
	if (vd->vdev_ops == &vdev_draid_ops &&
	    vdev_remove_empty_draid_spare_active(vd))
		return (SET_ERROR(EBUSY));

	return (0);
}

/*
 * Drop the distributed spares of a draid vdev from the pool's spares, so
 * that none is left behind without its vdev.  None of them is in use.
 */
static void
vdev_remove_empty_draid_spares(vdev_t *vd)
{
	spa_t *spa = vd->vdev_spa;
	spa_aux_vdev_t *sav = &spa->spa_spares;
	int count = sav->sav_count;
	int nguids = 0;

	if (count == 0)
		return;

	uint64_t *guids = kmem_alloc(count * sizeof (uint64_t), KM_SLEEP);
	for (int i = 0; i < count; i++) {
		vdev_t *svd = sav->sav_vdevs[i];

		if (svd->vdev_ops == &vdev_draid_spare_ops &&
		    vdev_draid_spare_get_parent(svd) == vd)
			guids[nguids++] = svd->vdev_guid;
	}

	/* Each removal replaces the array, so look it up every time. */
	for (int i = 0; i < nguids; i++) {
		nvlist_t **spares, *nv;
		uint_t nspares;

		if (nvlist_lookup_nvlist_array(sav->sav_config,
		    ZPOOL_CONFIG_SPARES, &spares, &nspares) != 0)
			break;
		nv = spa_nvlist_lookup_by_guid(spares, nspares, guids[i]);
		if (nv != NULL) {
			spa_vdev_remove_aux(sav->sav_config,
			    ZPOOL_CONFIG_SPARES, spares, nspares, nv);
		}
	}

	if (nguids > 0) {
		spa_load_spares(spa);
		sav->sav_sync = B_TRUE;
	}

	kmem_free(guids, count * sizeof (uint64_t));
}

typedef struct vdev_remove_empty_features {
	boolean_t	vref_draid;
	boolean_t	vref_draid_fail_domains;
	uint64_t	vref_raidz_expansions;
} vdev_remove_empty_features_t;

/*
 * Give back the feature references that the vdev took when it was added or
 * expanded, in the txg that writes the hole.
 */
static void
vdev_remove_empty_features_sync(void *arg, dmu_tx_t *tx)
{
	vdev_remove_empty_features_t *vref = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;

	if (vref->vref_draid)
		spa_feature_decr(spa, SPA_FEATURE_DRAID, tx);
	if (vref->vref_draid_fail_domains)
		spa_feature_decr(spa, SPA_FEATURE_DRAID_FAIL_DOMAINS, tx);
	for (uint64_t i = 0; i < vref->vref_raidz_expansions; i++)
		spa_feature_decr(spa, SPA_FEATURE_RAIDZ_EXPANSION, tx);

	kmem_free(vref, sizeof (*vref));
}

static void
vdev_remove_empty_features(vdev_t *vd, uint64_t txg)
{
	spa_t *spa = vd->vdev_spa;
	vdev_remove_empty_features_t *vref;

	vref = kmem_zalloc(sizeof (*vref), KM_SLEEP);
	if (vd->vdev_ops == &vdev_draid_ops) {
		vdev_draid_config_t *vdc = vd->vdev_tsd;

		/* As counted by vdev_draid_spare_create(). */
		vref->vref_draid = B_TRUE;
		vref->vref_draid_fail_domains =
		    (vdc->vdc_width > vdc->vdc_children);
	} else if (vd->vdev_ops == &vdev_raidz_ops) {
		vdev_raidz_t *vdrz = vd->vdev_tsd;

		/* Each completed expansion took one reference. */
		mutex_enter(&vdrz->vd_expand_lock);
		vref->vref_raidz_expansions =
		    avl_numnodes(&vdrz->vd_expand_txgs);
		mutex_exit(&vdrz->vd_expand_lock);
	}

	if (!vref->vref_draid && vref->vref_raidz_expansions == 0) {
		kmem_free(vref, sizeof (*vref));
		return;
	}

	dmu_tx_t *tx = dmu_tx_create_assigned(spa->spa_dsl_pool, txg);
	dsl_sync_task_nowait(spa->spa_dsl_pool,
	    vdev_remove_empty_features_sync, vref, tx);
	dmu_tx_commit(tx);
}

/*
 * Wipe the label of each leaf on its own, ignoring errors.  Stopping at the
 * first leaf that fails, as vdev_label_init() does, would leave stale labels
 * behind that the importers prefer to the hole in the config.
 */
static void
vdev_remove_empty_label_remove(vdev_t *vd)
{
	for (int c = 0; c < vd->vdev_children; c++)
		vdev_remove_empty_label_remove(vd->vdev_child[c]);

	if (vd->vdev_ops->vdev_op_leaf)
		(void) vdev_label_init(vd, 0, VDEV_LABEL_REMOVE);
}

/*
 * Remove an empty top-level vdev by turning it into a hole, the way a log
 * vdev is removed.  Nothing is copied, no indirect vdev is created.  The
 * config lock is held for the specified TXG.  On failure everything is undone
 * and the error is returned.
 */
static int
spa_vdev_remove_empty(vdev_t *vd, uint64_t *txg)
{
	spa_t *spa = vd->vdev_spa;
	boolean_t set_noalloc = B_FALSE;
	int error;

	ASSERT(vd == vd->vdev_top);
	ASSERT(!vd->vdev_islog);
	ASSERT(spa_namespace_held());

	error = spa_vdev_remove_empty_check(vd);
	if (error != 0)
		return (error);

	if (!vd->vdev_noalloc) {
		set_noalloc = B_TRUE;
		error = vdev_passivate(vd, txg);
		if (error != 0)
			return (error);
	}

	/*
	 * Stop any initializing and TRIM in progress, but leave the state
	 * as "active" so that it can resume if we back out.
	 */
	spa_vdev_config_exit(spa, NULL, *txg, 0, FTAG);

	vdev_initialize_stop_all(vd, VDEV_INITIALIZE_ACTIVE);
	vdev_trim_stop_all(vd, VDEV_TRIM_ACTIVE);
	vdev_autotrim_stop_wait(vd);
	vdev_rebuild_stop_wait(vd);

	*txg = spa_vdev_config_enter(spa);

	/*
	 * Things might have changed while the config lock was dropped.
	 * Check again, and make sure that the vdev is still empty: the
	 * vdev_sync() of the final txg relies on it.
	 */
	error = spa_vdev_remove_empty_check(vd);
	if (error != 0) {
		if (set_noalloc)
			vdev_activate(vd);
		spa_async_request(spa, SPA_ASYNC_INITIALIZE_RESTART);
		spa_async_request(spa, SPA_ASYNC_TRIM_RESTART);
		spa_async_request(spa, SPA_ASYNC_AUTOTRIM_RESTART);
		return (error);
	}

	/*
	 * From here on nothing can fail.  Take the vdev out of the pool's
	 * space, as vdev_remove_complete() does.  It stays non-allocating,
	 * which also keeps it out of the rotor if we crash before the hole
	 * is written.
	 */
	vdev_update_nonallocating_space(vd, B_FALSE);

	vd->vdev_removing = B_TRUE;
	vd->vdev_removing_empty = B_TRUE;

	vdev_dirty_leaves(vd, VDD_DTL, *txg);
	vdev_config_dirty(vd);

	/*
	 * As for a log vdev, call vdev_metaslab_fini() before the top ZAP
	 * is destroyed, and leave the metaslab groups to vdev_free().
	 */
	vdev_metaslab_fini(vd);

	spa_vdev_config_exit(spa, NULL, *txg, 0, FTAG);
	*txg = spa_vdev_config_enter(spa);

	sysevent_t *ev = spa_event_create(spa, vd, NULL,
	    ESC_ZFS_VDEV_REMOVE_DEV);
	ASSERT(spa_namespace_held());
	ASSERT(spa_config_held(spa, SCL_ALL, RW_WRITER) == SCL_ALL);

	/* The top ZAP should have been destroyed by vdev_remove_empty. */
	ASSERT0(vd->vdev_top_zap);
	/* The leaf ZAPs should have been destroyed by vdev_dtl_sync. */
	ASSERT0(vd->vdev_leaf_zap);

	vdev_remove_empty_label_remove(vd);

	if (list_link_active(&vd->vdev_state_dirty_node))
		vdev_state_clean(vd);
	if (list_link_active(&vd->vdev_config_dirty_node))
		vdev_config_clean(vd);

	ASSERT0(vd->vdev_stat.vs_alloc);

	/*
	 * The hole, the spares and the features are all written in this
	 * txg, so that a crash leaves either all of them or none.
	 */
	if (vd->vdev_ops == &vdev_draid_ops)
		vdev_remove_empty_draid_spares(vd);
	vdev_remove_empty_features(vd, *txg);

	vdev_remove_make_hole_and_free(vd);

	if (ev != NULL)
		spa_event_post(ev);

	return (0);
}

