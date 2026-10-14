// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

/*
 * Copyright (c) 2011, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 */

/*
 * zhack is a debugging tool that can write changes to ZFS pool using libzpool
 * for testing purposes. Altering pools with zhack is unsupported and may
 * result in corrupted pools.
 */

#include <zfs_prop.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/zfs_znode.h>
#include <sys/dsl_synctask.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/mmp.h>
#include <sys/fs/zfs.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/zio_checksum.h>
#include <sys/zio_compress.h>
#include <sys/zfeature.h>
#include <sys/dmu_tx.h>
#include <sys/backtrace.h>
#include <sys/dsl_dir.h>
#include <sys/space_map.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <sys/metaslab_impl.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/dsl_dataset.h>
#include <sys/sa.h>
#include <sys/sa_impl.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_project.h>
#include <libzpool.h>

static importargs_t g_importargs;
static char *g_pool;
static boolean_t g_readonly;
static boolean_t g_dump_dbgmsg;
/*
 * Set by drain when there is no /dev/zfs: the active-pool check of the
 * import needs it, and no kernel here can have the pool imported.
 */
static boolean_t g_no_kernel;

/*
 * In readonly imports, metaslab space maps are not opened unless this is set.
 * We temporarily enable it for leak scan/dry-run so referenced space maps are
 * accounted for accurately.
 */
extern boolean_t spa_mode_readable_spacemaps;

typedef enum {
	ZHACK_REPAIR_OP_UNKNOWN  = 0,
	ZHACK_REPAIR_OP_CKSUM    = (1 << 0),
	ZHACK_REPAIR_OP_UNDETACH = (1 << 1)
} zhack_repair_op_t;

static __attribute__((noreturn)) void
usage(void)
{
	(void) fprintf(stderr,
	    "Usage: zhack [-o tunable] [-c cachefile] [-d dir] [-G] "
	    "<subcommand> <args> ...\n"
	    "       where <subcommand> <args> is one of the following:\n"
	    "\n");

	(void) fprintf(stderr,
	    "    global options:\n"
	    "    -c <cachefile>   reads config from the given cachefile\n"
	    "    -d <dir>         directory with vdevs for import\n"
	    "    -o var=value...  set global variable to an unsigned "
	    "32-bit integer\n"
	    "    -G               dump zfs_dbgmsg buffer before exiting\n"
	    "\n"
	    "    action idle <pool> [-f] [-t seconds]\n"
	    "        import the pool for a set time then export it\n"
	    "        -t <seconds> sets the time the pool is imported\n"
	    "\n"
	    "    feature stat <pool>\n"
	    "        print information about enabled features\n"
	    "    feature enable [-r] [-d desc] <pool> <feature>\n"
	    "        add a new enabled feature to the pool\n"
	    "        -d <desc> sets the feature's description\n"
	    "        -r set read-only compatible flag for feature\n"
	    "    feature ref [-md] <pool> <feature>\n"
	    "        change the refcount on the given feature\n"
	    "        -d decrease instead of increase the refcount\n"
	    "        -m add the feature to the label if increasing refcount\n"
	    "\n"
	    "    <feature> : should be a feature guid\n"
	    "\n"
	    "    label repair <device>\n"
	    "        repair labels of a specified device according to options\n"
	    "        which may be combined to do their functions in one call\n"
	    "        -c repair corrupted label checksums\n"
	    "        -u restore the label on a detached device\n"
	    "\n"
	    "    <device> : path to vdev\n"
	    "\n"
	    "    mmp reclaim <pool>\n"
	    "        import a pool whose MMP claim cannot reach every mirror\n"
	    "        leg the config still expects, then mark the unreachable\n"
	    "        leaves offline so ordinary imports succeed.  Manual\n"
	    "        recovery: fence the peer first, this cannot see a\n"
	    "        live host whose legs are all invisible from here\n"
	    "\n"
	    "    metaslab leak <pool>\n"
	    "        apply allocation map from zdb to specified pool\n"
	    "\n"
	    "    mos scan <pool>\n"
	    "        identify reclaimable leaked MOS objects\n"
	    "    mos reclaim [-w] <pool>\n"
	    "        reclaim safe leaked MOS objects; dry-run unless -w\n"
	    "    mos leak [-w] [-c count] [-s count] <pool>\n"
	    "        create leaked MOS objects for testing\n"
	    "        -c <count> leaked DSL clone maps to create (default 1)\n"
	    "        -s <count> leaked SPA space maps to create (default 1)\n"
	    "        dry-run unless -w\n"
	    "\n"
	    "    drain [-cDKlMOSUvz] [-b KiB] [-n rounds] <pool>\n"
	    "        PROTOTYPE: rewrite every block with a copy on a\n"
	    "        top-level vdev with allocating=off, in all filesystems\n"
	    "        and volumes and in the MOS, until its allocated space\n"
	    "        stops decreasing\n"
	    "        -b <KiB> dirtied MOS bytes per txg (default 16384)\n"
	    "        -n <rounds> maximum number of rounds (default 8)\n"
	    "        -l logical rewrite (default: physical when enabled)\n"
	    "        -S skip blocks shared with a snapshot\n"
	    "        -D -U -M skip the dataset, used-object, MOS pass\n"
	    "        -v list the MOS objects touched\n"
	    "        -O account DMU_OST_OTHER objsets as ZPL ones (tests)\n"
	    "        -K do not dirty the MOS with the used objects (tests)\n"
	    "        -c walk the MOS during the dataset pass too (tests)\n"
	    "        -z start a scrub first (tests)\n");
	exit(1);
}

static void
dump_debug_buffer(void)
{
	ssize_t ret __attribute__((unused));

	if (!g_dump_dbgmsg)
		return;

	/*
	 * We use write() instead of printf() so that this function
	 * is safe to call from a signal handler.
	 */
	ret = write(STDERR_FILENO, "\n", 1);
	zfs_dbgmsg_print(STDERR_FILENO, "zhack");
}

static void sig_handler(int signo)
{
	struct sigaction action;

	libspl_backtrace(STDERR_FILENO);
	dump_debug_buffer();

	/*
	 * Restore default action and re-raise signal so SIGSEGV and
	 * SIGABRT can trigger a core dump.
	 */
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void) sigaction(signo, &action, NULL);
	raise(signo);
}

static __attribute__((format(printf, 3, 4))) __attribute__((noreturn)) void
fatal(spa_t *spa, const void *tag, const char *fmt, ...)
{
	va_list ap;

	if (spa != NULL) {
		spa_close(spa, tag);
		(void) spa_export(g_pool, NULL, B_TRUE, B_FALSE);
	}

	va_start(ap, fmt);
	(void) fputs("zhack: ", stderr);
	(void) vfprintf(stderr, fmt, ap);
	va_end(ap);
	(void) fputc('\n', stderr);

	dump_debug_buffer();

	exit(1);
}

static int
space_delta_cb(dmu_object_type_t bonustype, const void *data,
    zfs_file_info_t *zoi)
{
	(void) data, (void) zoi;

	/*
	 * Is it a valid type of object to track?
	 */
	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (ENOENT);
	(void) fprintf(stderr, "modifying object that needs user accounting");
	abort();
}

/*
 * Target is the dataset whose pool we want to open.
 */
static void
zhack_import(char *target, boolean_t readonly)
{
	nvlist_t *config;
	nvlist_t *props;
	int error;

	kernel_init(readonly ? SPA_MODE_READ :
	    (SPA_MODE_READ | SPA_MODE_WRITE));

	dmu_objset_register_type(DMU_OST_ZFS, space_delta_cb);

	g_readonly = readonly;
	g_importargs.can_be_active = readonly || g_no_kernel;
	g_pool = strdup(target);

	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	error = zpool_find_config(&lpch, target, &config, &g_importargs);
	if (error)
		fatal(NULL, FTAG, "cannot import '%s'", target);

	props = NULL;
	if (readonly) {
		VERIFY0(nvlist_alloc(&props, NV_UNIQUE_NAME, 0));
		VERIFY0(nvlist_add_uint64(props,
		    zpool_prop_to_name(ZPOOL_PROP_READONLY), 1));
	}

	zfeature_checks_disable = B_TRUE;
	error = spa_import(target, config, props,
	    (readonly ? ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	zfeature_checks_disable = B_FALSE;
	if (error == EEXIST)
		error = 0;

	if (error)
		fatal(NULL, FTAG, "can't import '%s': %s", target,
		    strerror(error));
}

static void
zhack_spa_open(char *target, boolean_t readonly, const void *tag, spa_t **spa)
{
	int err;

	zhack_import(target, readonly);

	zfeature_checks_disable = B_TRUE;
	err = spa_open(target, spa, tag);
	zfeature_checks_disable = B_FALSE;

	if (err != 0)
		fatal(*spa, FTAG, "cannot open '%s': %s", target,
		    strerror(err));
	if (spa_version(*spa) < SPA_VERSION_FEATURES) {
		fatal(*spa, FTAG, "'%s' has version %d, features not enabled",
		    target, (int)spa_version(*spa));
	}
}

static void
dump_obj(objset_t *os, uint64_t obj, const char *name)
{
	zap_cursor_t zc;
	zap_attribute_t *za = zap_attribute_long_alloc();

	(void) printf("%s_obj:\n", name);

	for (zap_cursor_init(&zc, os, obj);
	    zap_cursor_retrieve(&zc, za) == 0;
	    zap_cursor_advance(&zc)) {
		if (za->za_integer_length == 8) {
			ASSERT(za->za_num_integers == 1);
			(void) printf("\t%s = %llu\n",
			    za->za_name, (u_longlong_t)za->za_first_integer);
		} else {
			ASSERT(za->za_integer_length == 1);
			char val[1024];
			VERIFY0(zap_lookup(os, obj, za->za_name,
			    1, sizeof (val), val));
			(void) printf("\t%s = %s\n", za->za_name, val);
		}
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(za);
}

static void
dump_mos(spa_t *spa)
{
	nvlist_t *nv = spa->spa_label_features;
	nvpair_t *pair;

	(void) printf("label config:\n");
	for (pair = nvlist_next_nvpair(nv, NULL);
	    pair != NULL;
	    pair = nvlist_next_nvpair(nv, pair)) {
		(void) printf("\t%s\n", nvpair_name(pair));
	}
}

static void
zhack_do_feature_stat(int argc, char **argv)
{
	spa_t *spa;
	objset_t *os;
	char *target;

	argc--;
	argv++;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_TRUE, FTAG, &spa);
	os = spa->spa_meta_objset;

	dump_obj(os, spa->spa_feat_for_read_obj, "for_read");
	dump_obj(os, spa->spa_feat_for_write_obj, "for_write");
	dump_obj(os, spa->spa_feat_desc_obj, "descriptions");
	if (spa_feature_is_active(spa, SPA_FEATURE_ENABLED_TXG)) {
		dump_obj(os, spa->spa_feat_enabled_txg_obj, "enabled_txg");
	}
	dump_mos(spa);

	spa_close(spa, FTAG);
}

static void
zhack_feature_enable_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;

	feature_enable_sync(spa, feature, tx);

	spa_history_log_internal(spa, "zhack enable feature", tx,
	    "name=%s flags=%u",
	    feature->fi_guid, feature->fi_flags);
}

static void
zhack_do_feature_enable(int argc, char **argv)
{
	int c;
	char *desc, *target;
	spa_t *spa;
	objset_t *mos;
	zfeature_info_t feature;
	const spa_feature_t nodeps[] = { SPA_FEATURE_NONE };

	/*
	 * Features are not added to the pool's label until their refcounts
	 * are incremented, so fi_mos can just be left as false for now.
	 */
	desc = NULL;
	feature.fi_uname = "zhack";
	feature.fi_flags = 0;
	feature.fi_depends = nodeps;
	feature.fi_feature = SPA_FEATURE_NONE;

	optind = 1;
	while ((c = getopt(argc, argv, "+rd:")) != -1) {
		switch (c) {
		case 'r':
			feature.fi_flags |= ZFEATURE_FLAG_READONLY_COMPAT;
			break;
		case 'd':
			if (desc != NULL)
				free(desc);
			desc = strdup(optarg);
			break;
		default:
			usage();
			break;
		}
	}

	if (desc == NULL)
		desc = strdup("zhack injected");
	feature.fi_desc = desc;

	argc -= optind;
	argv += optind;

	if (argc < 2) {
		(void) fprintf(stderr, "error: missing feature or pool name\n");
		usage();
	}
	target = argv[0];
	feature.fi_guid = argv[1];

	if (!zfeature_is_valid_guid(feature.fi_guid))
		fatal(NULL, FTAG, "invalid feature guid: %s", feature.fi_guid);

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mos = spa->spa_meta_objset;

	if (zfeature_is_supported(feature.fi_guid))
		fatal(spa, FTAG, "'%s' is a real feature, will not enable",
		    feature.fi_guid);
	if (0 == zap_contains(mos, spa->spa_feat_desc_obj, feature.fi_guid))
		fatal(spa, FTAG, "feature already enabled: %s",
		    feature.fi_guid);

	VERIFY0(dsl_sync_task(spa_name(spa), NULL,
	    zhack_feature_enable_sync, &feature, 5, ZFS_SPACE_CHECK_NORMAL));

	spa_close(spa, FTAG);

	free(desc);
}

static void
feature_incr_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;
	uint64_t refcount;

	mutex_enter(&spa->spa_feat_stats_lock);
	VERIFY0(feature_get_refcount_from_disk(spa, feature, &refcount));
	feature_sync(spa, feature, refcount + 1, tx);
	spa_history_log_internal(spa, "zhack feature incr", tx,
	    "name=%s", feature->fi_guid);
	mutex_exit(&spa->spa_feat_stats_lock);
}

static void
feature_decr_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	zfeature_info_t *feature = arg;
	uint64_t refcount;

	mutex_enter(&spa->spa_feat_stats_lock);
	VERIFY0(feature_get_refcount_from_disk(spa, feature, &refcount));
	feature_sync(spa, feature, refcount - 1, tx);
	spa_history_log_internal(spa, "zhack feature decr", tx,
	    "name=%s", feature->fi_guid);
	mutex_exit(&spa->spa_feat_stats_lock);
}

static void
zhack_do_feature_ref(int argc, char **argv)
{
	int c;
	char *target;
	boolean_t decr = B_FALSE;
	spa_t *spa;
	objset_t *mos;
	zfeature_info_t feature;
	const spa_feature_t nodeps[] = { SPA_FEATURE_NONE };

	/*
	 * fi_desc does not matter here because it was written to disk
	 * when the feature was enabled, but we need to properly set the
	 * feature for read or write based on the information we read off
	 * disk later.
	 */
	feature.fi_uname = "zhack";
	feature.fi_flags = 0;
	feature.fi_desc = NULL;
	feature.fi_depends = nodeps;
	feature.fi_feature = SPA_FEATURE_NONE;

	optind = 1;
	while ((c = getopt(argc, argv, "+md")) != -1) {
		switch (c) {
		case 'm':
			feature.fi_flags |= ZFEATURE_FLAG_MOS;
			break;
		case 'd':
			decr = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2) {
		(void) fprintf(stderr, "error: missing feature or pool name\n");
		usage();
	}
	target = argv[0];
	feature.fi_guid = argv[1];

	if (!zfeature_is_valid_guid(feature.fi_guid))
		fatal(NULL, FTAG, "invalid feature guid: %s", feature.fi_guid);

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mos = spa->spa_meta_objset;

	if (zfeature_is_supported(feature.fi_guid)) {
		fatal(spa, FTAG,
		    "'%s' is a real feature, will not change refcount",
		    feature.fi_guid);
	}

	if (0 == zap_contains(mos, spa->spa_feat_for_read_obj,
	    feature.fi_guid)) {
		feature.fi_flags &= ~ZFEATURE_FLAG_READONLY_COMPAT;
	} else if (0 == zap_contains(mos, spa->spa_feat_for_write_obj,
	    feature.fi_guid)) {
		feature.fi_flags |= ZFEATURE_FLAG_READONLY_COMPAT;
	} else {
		fatal(spa, FTAG, "feature is not enabled: %s", feature.fi_guid);
	}

	if (decr) {
		uint64_t count;
		if (feature_get_refcount_from_disk(spa, &feature,
		    &count) == 0 && count == 0) {
			fatal(spa, FTAG, "feature refcount already 0: %s",
			    feature.fi_guid);
		}
	}

	VERIFY0(dsl_sync_task(spa_name(spa), NULL,
	    decr ? feature_decr_sync : feature_incr_sync, &feature,
	    5, ZFS_SPACE_CHECK_NORMAL));

	spa_close(spa, FTAG);
}

static int
zhack_do_feature(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no feature operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "stat") == 0) {
		zhack_do_feature_stat(argc, argv);
	} else if (strcmp(subcommand, "enable") == 0) {
		zhack_do_feature_enable(argc, argv);
	} else if (strcmp(subcommand, "ref") == 0) {
		zhack_do_feature_ref(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

static void
zhack_do_action_idle(int argc, char **argv)
{
	spa_t *spa;
	char *target, *tmp;
	int idle_time = 0;
	int c;

	optind = 1;
	while ((c = getopt(argc, argv, "+t:")) != -1) {
		switch (c) {
		case 't':
			idle_time = strtol(optarg, &tmp, 0);
			if (*tmp) {
				(void) fprintf(stderr, "error: time must "
				    "be an integer in seconds: %s\n", tmp);
				usage();
			}
			if (idle_time < 0) {
				(void) fprintf(stderr, "error: time must "
				    "not be negative: %d\n", idle_time);
				usage();
			}
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_FALSE, FTAG, &spa);

	fprintf(stdout, "Imported pool %s, idle for %d seconds\n",
	    target, idle_time);
	sleep(idle_time);

	spa_close(spa, FTAG);
}

/*
 * Collect the mirror legs this host could not open.  vdev_not_present is set
 * during import for any leaf whose open failed (vdev.c), and a leg in that
 * state is what the relaxed claim forgives, so it is also what has to be
 * marked offline for the ordinary imports which follow to succeed.
 */
static void
zhack_collect_absent(vdev_t *vd, uint64_t *guids, uint_t *n, uint_t max)
{
	/*
	 * Skip the same subtrees mmp_claim_uberblock_sync() skips.  The claim
	 * never counts these, so offlining them buys nothing, and offlining a
	 * log leg would drag in spa_reset_logs().  Pruned at the interior node
	 * because the log flag lives on the top-level vdev.
	 */
	if (vd->vdev_islog || vd->vdev_isspare || vd->vdev_isl2cache ||
	    vd->vdev_ishole || vd->vdev_ops == &vdev_indirect_ops)
		return;

	for (uint64_t c = 0; c < vd->vdev_children; c++)
		zhack_collect_absent(vd->vdev_child[c], guids, n, max);

	if (!vd->vdev_ops->vdev_op_leaf || !vd->vdev_not_present)
		return;

	/*
	 * Take only the legs the relaxed claim can forgive, which are the
	 * direct children of a top-level mirror: the relaxation lives in the
	 * nparity == 0 branch and walks that vdev's children.  A raidz or
	 * draid member is required as parity+1 in aggregate and never demanded
	 * individually, so an absent one does not raise the requirement and
	 * offlining it would be a persistent change that buys nothing.
	 */
	if (vd->vdev_parent != vd->vdev_top ||
	    vdev_get_nparity(vd->vdev_top) != 0)
		return;

	VERIFY3U(*n, <, max);
	guids[(*n)++] = vd->vdev_guid;
}

static int
zhack_do_mmp_reclaim(int argc, char **argv)
{
	spa_t *spa;
	char *target;
	uint64_t *guids;
	uint_t nguids = 0, max;
	int c, failed = 0;

	optind = 1;
	while ((c = getopt(argc, argv, "+")) != -1) {
		switch (c) {
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	/*
	 * Relax the claim for this import only.  The write, the wait and the
	 * re-read are untouched, so a competing importer which shares any
	 * visibility with us is still refused.
	 */
	mmp_claim_relaxed = B_TRUE;
	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	mmp_claim_relaxed = B_FALSE;

	/*
	 * Nothing to recover on a pool without multihost: no claim runs, so an
	 * ordinary import already succeeds with the absent leaves simply
	 * missing.  Offlining them here would be a permanent change to a pool
	 * that never needed this tool.
	 */
	if (!spa_multihost(spa)) {
		(void) fprintf(stderr, "%s: multihost is off, so no uberblock "
		    "claim runs and an ordinary import will succeed; refusing "
		    "to offline anything\n", target);
		spa_close(spa, FTAG);
		return (1);
	}

	/* Takes SCL_VDEV itself, so size the array before we hold it. */
	max = MAX(vdev_count_leaves(spa), 1);
	guids = kmem_zalloc(max * sizeof (uint64_t), KM_SLEEP);

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	zhack_collect_absent(spa->spa_root_vdev, guids, &nguids, max);
	spa_config_exit(spa, SCL_VDEV, FTAG);

	if (nguids == 0) {
		(void) fprintf(stdout, "%s: imported, no absent leaves to "
		    "mark offline\n", target);
	}

	for (uint_t i = 0; i < nguids; i++) {
		int error = vdev_offline(spa, guids[i], 0);

		if (error == 0) {
			(void) fprintf(stdout, "%s: marked absent leaf %llu "
			    "offline\n", target, (u_longlong_t)guids[i]);
			continue;
		}

		failed++;
		if (error == EBUSY) {
			(void) fprintf(stderr, "%s: leaf %llu holds data no "
			    "other leaf has, left online\n", target,
			    (u_longlong_t)guids[i]);
		} else {
			(void) fprintf(stderr, "%s: could not offline leaf "
			    "%llu: %s\n", target, (u_longlong_t)guids[i],
			    strerror(error));
		}
	}

	if (failed != 0) {
		/*
		 * Not "the pool still needs zhack": this run exports cleanly
		 * under our own hostid, so the next import here skips the
		 * activity check entirely.  The cost lands on the next host
		 * to take the pool, whose claim will count the leaves left
		 * online and refuse.
		 */
		(void) fprintf(stderr, "%s: %d absent leaves are still "
		    "online; a later import from another host will count "
		    "them and be refused\n", target, failed);
	}

	kmem_free(guids, max * sizeof (uint64_t));
	spa_close(spa, FTAG);

	return (failed == 0 ? 0 : 1);
}

static int
zhack_do_mmp(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no mmp operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "reclaim") == 0) {
		return (zhack_do_mmp_reclaim(argc, argv));
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

static int
zhack_do_action(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no import operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "idle") == 0) {
		zhack_do_action_idle(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}


static boolean_t
strstarts(const char *a, const char *b)
{
	return (strncmp(a, b, strlen(b)) == 0);
}

typedef struct zhack_objvec {
	uint64_t *zv_objs;
	size_t zv_len;
	size_t zv_cap;
} zhack_objvec_t;

typedef struct zhack_leak_report {
	zhack_objvec_t zlr_clones;
	zhack_objvec_t zlr_spacemaps;
	uint64_t zlr_unref_clones;
	uint64_t zlr_unref_nonempty_clones;
	uint64_t zlr_unref_spacemaps;
	uint64_t zlr_unref_nonempty_spacemaps;
} zhack_leak_report_t;

static void
zhack_objvec_add(zhack_objvec_t *vec, uint64_t obj)
{
	if (vec->zv_len == vec->zv_cap) {
		size_t new_cap = vec->zv_cap == 0 ? 16 : vec->zv_cap * 2;
		uint64_t *new_objs = realloc(vec->zv_objs,
		    new_cap * sizeof (*new_objs));
		if (new_objs == NULL)
			fatal(NULL, FTAG, "out of memory");
		vec->zv_objs = new_objs;
		vec->zv_cap = new_cap;
	}
	vec->zv_objs[vec->zv_len++] = obj;
}

static void
zhack_objvec_fini(zhack_objvec_t *vec)
{
	free(vec->zv_objs);
	vec->zv_objs = NULL;
	vec->zv_len = 0;
	vec->zv_cap = 0;
}

static void
zhack_leak_report_fini(zhack_leak_report_t *report)
{
	zhack_objvec_fini(&report->zlr_clones);
	zhack_objvec_fini(&report->zlr_spacemaps);
}

static void
zhack_mos_refd_once(zfs_range_tree_t *refd_objs, uint64_t object)
{
	if (object == 0 || zfs_range_tree_contains(refd_objs, object, 1))
		return;

	zfs_range_tree_add(refd_objs, object, 1);
}

static void
zhack_collect_dd_clones_refs(objset_t *mos, zfs_range_tree_t *clones_refd_objs)
{
	uint64_t object = 0;

	while (dmu_object_next(mos, &object, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		dmu_buf_t *db = NULL;
		int error;

		error = dmu_object_info(mos, object, &doi);
		if (error != 0 || doi.doi_bonus_type != DMU_OT_DSL_DIR)
			continue;

		error = dmu_bonus_hold(mos, object, FTAG, &db);
		if (error != 0) {
			(void) fprintf(stderr, "warning: object %" PRIu64
			    " bonus hold failed: %s\n", object,
			    strerror(error));
			continue;
		}

		if (db->db_size >= offsetof(dsl_dir_phys_t, dd_clones) +
		    sizeof (((dsl_dir_phys_t *)0)->dd_clones)) {
			dsl_dir_phys_t *dd = db->db_data;
			zhack_mos_refd_once(clones_refd_objs, dd->dd_clones);
		}
		dmu_buf_rele(db, FTAG);
	}
}

static void
zhack_collect_dtl_sm_refs(vdev_t *vd, zfs_range_tree_t *spacemap_refd_objs)
{
	if (vd->vdev_ops->vdev_op_leaf) {
		space_map_t *sm = vd->vdev_dtl_sm;

		if (sm != NULL &&
		    sm->sm_dbuf->db_size == sizeof (space_map_phys_t))
			zhack_mos_refd_once(spacemap_refd_objs,
			    space_map_object(sm));
		return;
	}

	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		zhack_collect_dtl_sm_refs(vd->vdev_child[c],
		    spacemap_refd_objs);
	}
}

static void
zhack_collect_metaslab_sm_refs(vdev_t *vd, zfs_range_tree_t *spacemap_refd_objs)
{
	if (vd->vdev_top == vd) {
		for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
			space_map_t *sm = vd->vdev_ms[m]->ms_sm;

			if (sm != NULL &&
			    sm->sm_dbuf->db_size == sizeof (space_map_phys_t))
				zhack_mos_refd_once(spacemap_refd_objs,
				    space_map_object(sm));
		}
	}

	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		zhack_collect_metaslab_sm_refs(vd->vdev_child[c],
		    spacemap_refd_objs);
	}
}

static void
zhack_collect_obsolete_sm_refs(vdev_t *vd, zfs_range_tree_t *spacemap_refd_objs)
{
	uint64_t obsolete_sm_object;

	VERIFY0(vdev_obsolete_sm_object(vd, &obsolete_sm_object));
	if (vd->vdev_top == vd && obsolete_sm_object != 0) {
		dmu_object_info_t doi;

		if (dmu_object_info(vd->vdev_spa->spa_meta_objset,
		    obsolete_sm_object, &doi) == 0 &&
		    doi.doi_bonus_size == sizeof (space_map_phys_t)) {
			zhack_mos_refd_once(spacemap_refd_objs,
			    obsolete_sm_object);
		}
	}

	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		zhack_collect_obsolete_sm_refs(vd->vdev_child[c],
		    spacemap_refd_objs);
	}
}

static void
zhack_collect_prev_obsolete_sm_refs(spa_t *spa,
    zfs_range_tree_t *spacemap_refd_objs)
{
	uint64_t prev_obj =
	    spa->spa_condensing_indirect_phys.scip_prev_obsolete_sm_object;

	if (prev_obj != 0) {
		dmu_object_info_t doi;

		if (dmu_object_info(spa->spa_meta_objset, prev_obj,
		    &doi) == 0 &&
		    doi.doi_bonus_size == sizeof (space_map_phys_t)) {
			zhack_mos_refd_once(spacemap_refd_objs, prev_obj);
		}
	}
}

static void
zhack_collect_checkpoint_sm_refs(vdev_t *vd,
    zfs_range_tree_t *spacemap_refd_objs)
{
	if (vd->vdev_top == vd && vd->vdev_top_zap != 0) {
		uint64_t checkpoint_sm_obj;
		int error = zap_lookup(spa_meta_objset(vd->vdev_spa),
		    vd->vdev_top_zap, VDEV_TOP_ZAP_POOL_CHECKPOINT_SM,
		    sizeof (checkpoint_sm_obj), 1, &checkpoint_sm_obj);
		if (error == 0)
			zhack_mos_refd_once(spacemap_refd_objs,
			    checkpoint_sm_obj);
	}

	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		zhack_collect_checkpoint_sm_refs(vd->vdev_child[c],
		    spacemap_refd_objs);
	}
}

static void
zhack_collect_log_sm_refs(spa_t *spa, zfs_range_tree_t *spacemap_refd_objs)
{
	for (spa_log_sm_t *sls = avl_first(&spa->spa_sm_logs_by_txg);
	    sls != NULL;
	    sls = AVL_NEXT(&spa->spa_sm_logs_by_txg, sls)) {
		zhack_mos_refd_once(spacemap_refd_objs, sls->sls_sm_obj);
	}
}

static void
zhack_collect_spacemap_refs(spa_t *spa, zfs_range_tree_t *spacemap_refd_objs)
{
	zhack_collect_dtl_sm_refs(spa->spa_root_vdev, spacemap_refd_objs);
	zhack_collect_metaslab_sm_refs(spa->spa_root_vdev, spacemap_refd_objs);
	zhack_collect_obsolete_sm_refs(spa->spa_root_vdev, spacemap_refd_objs);
	zhack_collect_prev_obsolete_sm_refs(spa, spacemap_refd_objs);
	zhack_collect_checkpoint_sm_refs(spa->spa_root_vdev,
	    spacemap_refd_objs);
	zhack_collect_log_sm_refs(spa, spacemap_refd_objs);
}

static void
zhack_collect_mos_leak_report(spa_t *spa, zhack_leak_report_t *report,
    boolean_t print_details)
{
	objset_t *mos = spa->spa_meta_objset;
	zfs_range_tree_t *clones_refd_objs = zfs_range_tree_create_flags(
	    NULL, ZFS_RANGE_SEG64, NULL, 0, 0, 0,
	    "zhack_collect_mos_leak_report:clones_refd_objs");
	zfs_range_tree_t *spacemap_refd_objs = zfs_range_tree_create_flags(
	    NULL, ZFS_RANGE_SEG64, NULL, 0, 0, 0,
	    "zhack_collect_mos_leak_report:spacemap_refd_objs");
	uint64_t object = 0;

	spa_config_enter(spa, SCL_VDEV | SCL_ALLOC, FTAG, RW_READER);
	zhack_collect_spacemap_refs(spa, spacemap_refd_objs);
	spa_config_exit(spa, SCL_VDEV | SCL_ALLOC, FTAG);

	zhack_collect_dd_clones_refs(mos, clones_refd_objs);

	while (dmu_object_next(mos, &object, B_FALSE, 0) == 0) {
		dmu_object_info_t doi;
		int error = dmu_object_info(mos, object, &doi);
		if (error != 0)
			continue;

		if (doi.doi_type == DMU_OT_DSL_CLONES &&
		    !zfs_range_tree_contains(clones_refd_objs, object, 1)) {
			uint64_t entries = 0;

			report->zlr_unref_clones++;
			error = zap_count(mos, object, &entries);
			if (error == 0 && entries == 0) {
				zhack_objvec_add(&report->zlr_clones, object);
				if (print_details) {
					(void) printf("candidate: object %"
					    PRIu64 " type=DSL dir clones "
					    "entries=0\n", object);
				}
			} else {
				report->zlr_unref_nonempty_clones++;
				if (print_details) {
					if (error != 0) {
						(void) printf("skip: object %"
						    PRIu64 " type=DSL dir "
						    "clones (count failed: "
						    "%s)\n", object,
						    strerror(error));
					} else {
						(void) printf("skip: object %"
						    PRIu64 " type=DSL dir "
						    "clones entries=%" PRIu64
						    "\n", object, entries);
					}
				}
			}
			continue;
		}

		if (doi.doi_type == DMU_OT_SPACE_MAP &&
		    doi.doi_bonus_size == sizeof (space_map_phys_t) &&
		    !zfs_range_tree_contains(spacemap_refd_objs, object, 1)) {
			dmu_buf_t *db = NULL;
			uint64_t smp_alloc = 0;
			uint64_t smp_length = 0;

			report->zlr_unref_spacemaps++;
			error = dmu_bonus_hold(mos, object, FTAG, &db);
			if (error == 0) {
				space_map_phys_t *smp = db->db_data;
				smp_alloc = smp->smp_alloc;
				smp_length = smp->smp_length;
				dmu_buf_rele(db, FTAG);
			}

			if (error == 0 && smp_alloc == 0 && smp_length == 0) {
				zhack_objvec_add(&report->zlr_spacemaps,
				    object);
				if (print_details) {
					(void) printf("candidate: object %"
					    PRIu64 " type=SPA space map "
					    "smp_alloc=0x%llx "
					    "smp_length=0x%llx\n",
					    object, (u_longlong_t)smp_alloc,
					    (u_longlong_t)smp_length);
				}
			} else {
				report->zlr_unref_nonempty_spacemaps++;
				if (print_details) {
					if (error != 0) {
						(void) printf("skip: object "
						    "%" PRIu64 " type=SPA "
						    "space map (bonus hold "
						    "failed: %s)\n", object,
						    strerror(error));
					} else {
						(void) printf("skip: object "
						    "%" PRIu64 " type=SPA "
						    "space map "
						    "smp_alloc=0x%llx "
						    "smp_length=0x%llx\n",
						    object,
						    (u_longlong_t)smp_alloc,
						    (u_longlong_t)smp_length);
					}
				}
			}
		}
	}

	zfs_range_tree_vacate(clones_refd_objs, NULL, NULL);
	zfs_range_tree_destroy(clones_refd_objs);
	zfs_range_tree_vacate(spacemap_refd_objs, NULL, NULL);
	zfs_range_tree_destroy(spacemap_refd_objs);
}

static void
zhack_print_mos_leak_summary(const zhack_leak_report_t *report)
{
	(void) printf("summary:\n");
	(void) printf("\tunreferenced DSL dir clones: %" PRIu64
	    " (reclaimable=%llu skipped=%" PRIu64 ")\n",
	    report->zlr_unref_clones,
	    (u_longlong_t)report->zlr_clones.zv_len,
	    report->zlr_unref_nonempty_clones);
	(void) printf("\tunreferenced SPA space maps: %" PRIu64
	    " (reclaimable=%llu skipped=%" PRIu64 ")\n",
	    report->zlr_unref_spacemaps,
	    (u_longlong_t)report->zlr_spacemaps.zv_len,
	    report->zlr_unref_nonempty_spacemaps);
	(void) printf("\ttotal reclaimable objects: %llu\n",
	    (u_longlong_t)(report->zlr_clones.zv_len +
	    report->zlr_spacemaps.zv_len));
}

static void
zhack_leak_reclaim_sync(void *arg, dmu_tx_t *tx)
{
	zhack_leak_report_t *report = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	objset_t *mos = spa->spa_meta_objset;

	for (size_t i = 0; i < report->zlr_clones.zv_len; i++) {
		VERIFY0(zap_destroy(mos, report->zlr_clones.zv_objs[i], tx));
	}

	for (size_t i = 0; i < report->zlr_spacemaps.zv_len; i++) {
		space_map_free_obj(mos, report->zlr_spacemaps.zv_objs[i], tx);
	}

	spa_history_log_internal(spa, "zhack mos reclaim", tx,
	    "clones=%llu spacemaps=%llu",
	    (u_longlong_t)report->zlr_clones.zv_len,
	    (u_longlong_t)report->zlr_spacemaps.zv_len);
}

typedef struct zhack_mos_leak_arg {
	uint64_t zli_clone_count;
	uint64_t zli_spacemap_count;
	uint64_t *zli_clone_objs;
	uint64_t *zli_spacemap_objs;
} zhack_mos_leak_arg_t;

static void
zhack_mos_leak_sync(void *arg, dmu_tx_t *tx)
{
	zhack_mos_leak_arg_t *inject = arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	objset_t *mos = spa->spa_meta_objset;

	for (uint64_t i = 0; i < inject->zli_clone_count; i++) {
		inject->zli_clone_objs[i] = zap_create(mos, DMU_OT_DSL_CLONES,
		    DMU_OT_NONE, 0, tx);
	}
	for (uint64_t i = 0; i < inject->zli_spacemap_count; i++) {
		inject->zli_spacemap_objs[i] = space_map_alloc(mos,
		    SPA_MINBLOCKSIZE, tx);
	}

	spa_history_log_internal(spa, "zhack mos leak", tx,
	    "clones=%llu spacemaps=%llu",
	    (u_longlong_t)inject->zli_clone_count,
	    (u_longlong_t)inject->zli_spacemap_count);
}

static int
zhack_do_mos_scan(int argc, char **argv)
{
	zhack_leak_report_t report = { 0 };
	spa_t *spa;
	char *target;
	boolean_t readable_spacemaps_saved;

	argc--;
	argv++;
	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	readable_spacemaps_saved = spa_mode_readable_spacemaps;
	spa_mode_readable_spacemaps = B_TRUE;
	zhack_spa_open(target, B_TRUE, FTAG, &spa);
	spa_mode_readable_spacemaps = readable_spacemaps_saved;

	zhack_collect_mos_leak_report(spa, &report, B_TRUE);
	zhack_print_mos_leak_summary(&report);
	zhack_leak_report_fini(&report);
	spa_close(spa, FTAG);

	return (0);
}

static int
zhack_do_mos_reclaim(int argc, char **argv)
{
	zhack_leak_report_t report = { 0 };
	spa_t *spa;
	char *target;
	int c;
	boolean_t do_write = B_FALSE;
	boolean_t readable_spacemaps_saved;

	optind = 1;
	while ((c = getopt(argc, argv, "+w")) != -1) {
		switch (c) {
		case 'w':
			do_write = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	readable_spacemaps_saved = spa_mode_readable_spacemaps;
	if (!do_write)
		spa_mode_readable_spacemaps = B_TRUE;
	zhack_spa_open(target, !do_write, FTAG, &spa);
	spa_mode_readable_spacemaps = readable_spacemaps_saved;

	zhack_collect_mos_leak_report(spa, &report, B_TRUE);
	zhack_print_mos_leak_summary(&report);

	if (report.zlr_clones.zv_len == 0 && report.zlr_spacemaps.zv_len == 0) {
		(void) printf("nothing to reclaim\n");
		zhack_leak_report_fini(&report);
		spa_close(spa, FTAG);
		return (0);
	}

	if (!do_write) {
		(void) printf("dry run: no changes made "
		    "(re-run with -w to reclaim)\n");
		zhack_leak_report_fini(&report);
		spa_close(spa, FTAG);
		return (0);
	}

	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_leak_reclaim_sync,
	    &report, 5, ZFS_SPACE_CHECK_NORMAL));
	(void) printf("reclaimed %llu leaked MOS objects\n",
	    (u_longlong_t)(report.zlr_clones.zv_len +
	    report.zlr_spacemaps.zv_len));
	zhack_leak_report_fini(&report);
	spa_close(spa, FTAG);

	return (0);
}

static int
zhack_do_mos_leak(int argc, char **argv)
{
	zhack_mos_leak_arg_t inject = {
		.zli_clone_count = 1,
		.zli_spacemap_count = 1,
		.zli_clone_objs = NULL,
		.zli_spacemap_objs = NULL
	};
	spa_t *spa;
	char *target, *tmp;
	int c;
	boolean_t do_write = B_FALSE;

	optind = 1;
	while ((c = getopt(argc, argv, "+c:s:w")) != -1) {
		switch (c) {
		case 'c':
			if (optarg[0] == '-') {
				(void) fprintf(stderr, "error: invalid clone "
				    "count: %s\n", optarg);
				usage();
			}
			inject.zli_clone_count = strtoull(optarg, &tmp, 0);
			if (*tmp != '\0') {
				(void) fprintf(stderr, "error: invalid clone "
				    "count: %s\n", optarg);
				usage();
			}
			break;
		case 's':
			if (optarg[0] == '-') {
				(void) fprintf(stderr, "error: invalid space "
				    "map count: %s\n", optarg);
				usage();
			}
			inject.zli_spacemap_count = strtoull(optarg, &tmp, 0);
			if (*tmp != '\0') {
				(void) fprintf(stderr, "error: invalid space "
				    "map count: %s\n", optarg);
				usage();
			}
			break;
		case 'w':
			do_write = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	if (inject.zli_clone_count == 0 && inject.zli_spacemap_count == 0) {
		(void) printf("nothing requested to leak\n");
		return (0);
	}

	if (!do_write) {
		zhack_spa_open(target, B_TRUE, FTAG, &spa);
		spa_close(spa, FTAG);
		(void) printf("dry run: would create %llu leaked DSL clone "
		    "map(s) and %llu leaked SPA space map(s)\n",
		    (u_longlong_t)inject.zli_clone_count,
		    (u_longlong_t)inject.zli_spacemap_count);
		(void) printf("(re-run with -w to apply)\n");
		return (0);
	}

	zhack_spa_open(target, B_FALSE, FTAG, &spa);

	if (inject.zli_clone_count != 0) {
		inject.zli_clone_objs = calloc(inject.zli_clone_count,
		    sizeof (*inject.zli_clone_objs));
		if (inject.zli_clone_objs == NULL)
			fatal(spa, FTAG, "out of memory");
	}
	if (inject.zli_spacemap_count != 0) {
		inject.zli_spacemap_objs = calloc(inject.zli_spacemap_count,
		    sizeof (*inject.zli_spacemap_objs));
		if (inject.zli_spacemap_objs == NULL)
			fatal(spa, FTAG, "out of memory");
	}

	VERIFY0(dsl_sync_task(spa_name(spa), NULL, zhack_mos_leak_sync,
	    &inject, 5, ZFS_SPACE_CHECK_NORMAL));

	for (uint64_t i = 0; i < inject.zli_clone_count; i++) {
		(void) printf("leaked: object %" PRIu64
		    " type=DSL dir clones entries=0\n",
		    inject.zli_clone_objs[i]);
	}
	for (uint64_t i = 0; i < inject.zli_spacemap_count; i++) {
		(void) printf("leaked: object %" PRIu64
		    " type=SPA space map smp_alloc=0x0 smp_length=0x0\n",
		    inject.zli_spacemap_objs[i]);
	}
	(void) printf("leaked summary: clones=%llu spacemaps=%llu "
	    "total=%llu\n",
	    (u_longlong_t)inject.zli_clone_count,
	    (u_longlong_t)inject.zli_spacemap_count,
	    (u_longlong_t)(inject.zli_clone_count +
	    inject.zli_spacemap_count));

	free(inject.zli_clone_objs);
	free(inject.zli_spacemap_objs);
	spa_close(spa, FTAG);
	return (0);
}

static int
zhack_do_mos(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no mos operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "scan") == 0) {
		return (zhack_do_mos_scan(argc, argv));
	} else if (strcmp(subcommand, "reclaim") == 0) {
		return (zhack_do_mos_reclaim(argc, argv));
	} else if (strcmp(subcommand, "leak") == 0) {
		return (zhack_do_mos_leak(argc, argv));
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

static void
metaslab_force_alloc(metaslab_t *msp, uint64_t start, uint64_t size,
    dmu_tx_t *tx)
{
	ASSERT(msp->ms_disabled);
	ASSERT(MUTEX_HELD(&msp->ms_lock));
	uint64_t txg = dmu_tx_get_txg(tx);

	uint64_t off = start;
	while (off < start + size) {
		uint64_t ostart, osize;
		boolean_t found = zfs_range_tree_find_in(msp->ms_allocatable,
		    off, start + size - off, &ostart, &osize);
		if (!found)
			break;
		zfs_range_tree_remove(msp->ms_allocatable, ostart, osize);

		if (zfs_range_tree_is_empty(msp->ms_allocating[txg & TXG_MASK]))
			vdev_dirty(msp->ms_group->mg_vd, VDD_METASLAB, msp,
			    txg);

		zfs_range_tree_add(msp->ms_allocating[txg & TXG_MASK], ostart,
		    osize);
		msp->ms_allocating_total += osize;
		off = ostart + osize;
	}
}

static void
zhack_do_metaslab_leak(int argc, char **argv)
{
	int c;
	char *target;
	spa_t *spa;

	optind = 1;
	boolean_t force = B_FALSE;
	while ((c = getopt(argc, argv, "f")) != -1) {
		switch (c) {
		case 'f':
			force = B_TRUE;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	spa_config_enter(spa, SCL_VDEV | SCL_ALLOC, FTAG, RW_READER);

	char *line = NULL;
	size_t cap = 0;

	vdev_t *vd = NULL;
	metaslab_t *prev = NULL;
	dmu_tx_t *tx = NULL;
	while (getline(&line, &cap, stdin) > 0) {
		if (strstarts(line, "\tvdev ")) {
			uint64_t vdev_id, ms_shift;
			if (sscanf(line,
			    "\tvdev %10"PRIu64"\t%*s  metaslab shift %4"PRIu64,
			    &vdev_id, &ms_shift) == 1) {
				VERIFY3U(sscanf(line, "\tvdev %"PRIu64
				    "\t  metaslab shift %4"PRIu64,
				    &vdev_id, &ms_shift), ==, 2);
			}
			vd = vdev_lookup_top(spa, vdev_id);
			if (vd == NULL) {
				fprintf(stderr, "error: no such vdev with "
				    "id %"PRIu64"\n", vdev_id);
				break;
			}
			if (tx) {
				dmu_tx_commit(tx);
				mutex_exit(&prev->ms_lock);
				metaslab_enable(prev, B_FALSE, B_FALSE);
				tx = NULL;
				prev = NULL;
			}
			if (vd->vdev_ms_shift != ms_shift) {
				fprintf(stderr, "error: ms_shift mismatch: %"
				    PRIu64" != %"PRIu64"\n", vd->vdev_ms_shift,
				    ms_shift);
				break;
			}
		} else if (strstarts(line, "\tmetaslabs ")) {
			uint64_t ms_count;
			VERIFY3U(sscanf(line, "\tmetaslabs %"PRIu64, &ms_count),
			    ==, 1);
			ASSERT(vd);
			if (!force && vd->vdev_ms_count != ms_count) {
				fprintf(stderr, "error: ms_count mismatch: %"
				    PRIu64" != %"PRIu64"\n", vd->vdev_ms_count,
				    ms_count);
				break;
			}
		} else if (strstarts(line, "ALLOC:")) {
			uint64_t start, size;
			VERIFY3U(sscanf(line, "ALLOC: %"PRIu64" %"PRIu64"\n",
			    &start, &size), ==, 2);

			ASSERT(vd);
			size_t idx;
			idx = start >> vd->vdev_ms_shift;
			if (idx >= vd->vdev_ms_count)
				continue;
			metaslab_t *cur = vd->vdev_ms[idx];
			if (prev != cur) {
				if (prev) {
					dmu_tx_commit(tx);
					mutex_exit(&prev->ms_lock);
					metaslab_enable(prev, B_FALSE, B_FALSE);
				}
				ASSERT(cur);
				metaslab_disable(cur);
				mutex_enter(&cur->ms_lock);
				metaslab_load(cur);
				prev = cur;
				tx = dmu_tx_create_dd(
				    spa_get_dsl(vd->vdev_spa)->dp_root_dir);
				dmu_tx_assign(tx, DMU_TX_WAIT);
			}

			metaslab_force_alloc(cur, start, size, tx);
		} else {
			continue;
		}
	}
	if (tx) {
		dmu_tx_commit(tx);
		mutex_exit(&prev->ms_lock);
		metaslab_enable(prev, B_FALSE, B_FALSE);
		tx = NULL;
		prev = NULL;
	}
	if (line)
		free(line);

	spa_config_exit(spa, SCL_VDEV | SCL_ALLOC, FTAG);
	spa_close(spa, FTAG);
}

static int
zhack_do_metaslab(int argc, char **argv)
{
	char *subcommand;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no metaslab operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "leak") == 0) {
		zhack_do_metaslab_leak(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (0);
}

/*
 * zhack drain: PROTOTYPE, never meant to be merged as is.
 *
 * Try to empty every top-level vdev with allocating=off (vdev_noalloc) by
 * writing every block that has a copy on one of them again, through the
 * DMU, so that the new copy is allocated elsewhere and the old one freed:
 *
 *  - every filesystem and volume (not snapshots) is owned, and each of its
 *    objects is walked: an L0 block with a DVA on such a vdev is marked for
 *    rewrite (dmu_rewrite_by_dnode()), an indirect block on such a vdev
 *    with no marked L0 under it is dirtied on its own, a dnode block on
 *    such a vdev is moved by dnode_setdirty() of an object in it (or by
 *    dirtying the block when it holds no allocated dnode), the spill block
 *    is dirtied, and an objset whose root block is there is dirtied;
 *  - the user/group/project used objects, which only syncing context may
 *    touch, are walked from a sync task per dataset;
 *  - the MOS is walked from sync tasks, at most dr_mos_budget bytes of
 *    dirtied blocks per txg, and every block on such a vdev is dirtied.
 *
 * Rounds repeat until the allocated space of those vdevs stops decreasing.
 * Snapshots and the blocks they share keep their copies: nothing here can
 * move them.
 */

#define	DRAIN_NO_L1	UINT64_MAX
#define	DRAIN_NTYPES	(DMU_OT_NUMTYPES + 128)
#define	DRAIN_RUN	16

typedef struct drain_unit {
	uint64_t	du_l1;		/* L1 block id, DRAIN_NO_L1 if none */
	boolean_t	du_hit;		/* L1 or an ancestor is to move */
	size_t		du_first;	/* index of its first L0 in sel_blk */
	size_t		du_count;	/* number of its L0s in sel_blk */
} drain_unit_t;

typedef struct drain_sel {
	uint64_t	*sel_blk;
	size_t		sel_nblk;
	size_t		sel_maxblk;
	drain_unit_t	*sel_unit;
	size_t		sel_nunit;
	size_t		sel_maxunit;
	uint64_t	sel_errors;
} drain_sel_t;

typedef struct drain_cnt {
	uint64_t	dc_objsets;
	uint64_t	dc_objects;
	uint64_t	dc_l0;
	uint64_t	dc_l0_bytes;
	uint64_t	dc_l1;
	uint64_t	dc_dnode;
	uint64_t	dc_mdn;
	uint64_t	dc_spill;
	uint64_t	dc_root;
	uint64_t	dc_used;
	uint64_t	dc_used_objs;
	uint64_t	dc_bytes;
	uint64_t	dc_errors;
	uint64_t	dc_txgs;
	uint64_t	dc_type_blocks[DRAIN_NTYPES];
	uint64_t	dc_type_bytes[DRAIN_NTYPES];
} drain_cnt_t;

typedef struct drain_ctx {
	spa_t		*dr_spa;
	uint8_t		*dr_noalloc;
	uint64_t	dr_nvdevs;
	uint64_t	dr_rwflags;
	uint64_t	dr_mos_budget;
	boolean_t	dr_verbose;
	boolean_t	dr_no_mos_kick;
} drain_ctx_t;

static drain_ctx_t g_drain;

typedef struct drain_mos {
	uint64_t	dm_cursor;
	boolean_t	dm_done;
	drain_cnt_t	*dm_cnt;
} drain_mos_t;

typedef struct drain_used {
	const char	*du_name;
	drain_cnt_t	*du_cnt;
} drain_used_t;

typedef struct drain_names {
	char		**dn_name;
	int		dn_n;
	int		dn_max;
} drain_names_t;

/*
 * The user/group/project accounting callback for ZPL objsets, a copy of
 * zpl_get_file_info(), which libzpool does not build.  zhack's own
 * callback aborts, and none at all would leave the accounting stale.
 */
static int
drain_get_file_info(dmu_object_type_t bonustype, const void *data,
    zfs_file_info_t *zoi)
{
	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (SET_ERROR(ENOENT));

	zoi->zfi_project = ZFS_DEFAULT_PROJID;
	if (data == NULL)
		return (SET_ERROR(EEXIST));

	if (bonustype == DMU_OT_ZNODE) {
		const znode_phys_t *znp = data;
		zoi->zfi_user = znp->zp_uid;
		zoi->zfi_group = znp->zp_gid;
		zoi->zfi_generation = znp->zp_gen;
		return (0);
	}

	const sa_hdr_phys_t *sap = data;
	if (sap->sa_magic == 0) {
		zoi->zfi_user = 0;
		zoi->zfi_group = 0;
		zoi->zfi_generation = 0;
		return (0);
	}

	sa_hdr_phys_t sa = *sap;
	boolean_t swap = B_FALSE;
	if (sa.sa_magic == BSWAP_32(SA_MAGIC)) {
		sa.sa_magic = SA_MAGIC;
		sa.sa_layout_info = BSWAP_16(sa.sa_layout_info);
		swap = B_TRUE;
	}
	if (sa.sa_magic != SA_MAGIC)
		return (SET_ERROR(EINVAL));

	int hdrsize = sa_hdrsize(&sa);
	if (hdrsize < (int)sizeof (sa_hdr_phys_t))
		return (SET_ERROR(EINVAL));

	uintptr_t p = (uintptr_t)data + hdrsize;
	zoi->zfi_user = *((uint64_t *)(p + SA_UID_OFFSET));
	zoi->zfi_group = *((uint64_t *)(p + SA_GID_OFFSET));
	zoi->zfi_generation = *((uint64_t *)(p + SA_GEN_OFFSET));
	uint64_t flags = *((uint64_t *)(p + SA_FLAGS_OFFSET));
	if (swap)
		flags = BSWAP_64(flags);
	if (flags & ZFS_PROJID)
		zoi->zfi_project = *((uint64_t *)(p + SA_PROJID_OFFSET));
	if (swap) {
		zoi->zfi_user = BSWAP_64(zoi->zfi_user);
		zoi->zfi_group = BSWAP_64(zoi->zfi_group);
		zoi->zfi_project = BSWAP_64(zoi->zfi_project);
		zoi->zfi_generation = BSWAP_64(zoi->zfi_generation);
	}
	return (0);
}

static boolean_t
drain_bp_hit(const blkptr_t *bp)
{
	if (BP_IS_HOLE(bp) || BP_IS_EMBEDDED(bp))
		return (B_FALSE);
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		uint64_t id = DVA_GET_VDEV(&bp->blk_dva[d]);
		if (id < g_drain.dr_nvdevs && g_drain.dr_noalloc[id])
			return (B_TRUE);
	}
	return (B_FALSE);
}

static int
drain_type_index(dmu_object_type_t t)
{
	if (t < DMU_OT_NUMTYPES)
		return (t);
	return (DMU_OT_NUMTYPES + (t & 0x7f));
}

static void
drain_type_name(int i, char *buf, size_t len)
{
	if (i < DMU_OT_NUMTYPES) {
		(void) strlcpy(buf, dmu_ot[i].ot_name, len);
	} else {
		int t = i - DMU_OT_NUMTYPES;
		(void) snprintf(buf, len, "newtype %s%s",
		    dmu_ot_byteswap[t & DMU_OT_BYTESWAP_MASK].ob_name,
		    (t & DMU_OT_METADATA) ? " metadata" : "");
	}
}

static void
drain_count_type(drain_cnt_t *cnt, dmu_object_type_t t, uint64_t blocks,
    uint64_t bytes)
{
	int i = drain_type_index(t);

	cnt->dc_type_blocks[i] += blocks;
	cnt->dc_type_bytes[i] += bytes;
	cnt->dc_bytes += bytes;
}

static size_t
drain_sel_unit(drain_sel_t *sel, uint64_t l1, boolean_t hit)
{
	if (sel->sel_nunit == sel->sel_maxunit) {
		sel->sel_maxunit = MAX(16, sel->sel_maxunit * 2);
		sel->sel_unit = realloc(sel->sel_unit,
		    sel->sel_maxunit * sizeof (drain_unit_t));
		VERIFY3P(sel->sel_unit, !=, NULL);
	}
	drain_unit_t *u = &sel->sel_unit[sel->sel_nunit];
	u->du_l1 = l1;
	u->du_hit = hit;
	u->du_first = sel->sel_nblk;
	u->du_count = 0;
	return (sel->sel_nunit++);
}

static void
drain_sel_blk(drain_sel_t *sel, size_t u, uint64_t blkid)
{
	if (sel->sel_nblk == sel->sel_maxblk) {
		sel->sel_maxblk = MAX(64, sel->sel_maxblk * 2);
		sel->sel_blk = realloc(sel->sel_blk,
		    sel->sel_maxblk * sizeof (uint64_t));
		VERIFY3P(sel->sel_blk, !=, NULL);
	}
	sel->sel_blk[sel->sel_nblk++] = blkid;
	sel->sel_unit[u].du_count++;
}

static void
drain_sel_fini(drain_sel_t *sel)
{
	free(sel->sel_blk);
	free(sel->sel_unit);
	memset(sel, 0, sizeof (*sel));
}

/*
 * Walk the indirect block (level, blkid) of dn.  hit is true when this
 * block or one of its ancestors has a DVA on a noalloc vdev.  The caller
 * holds dn_struct_rwlock as reader.
 */
static void
drain_walk_ind(dnode_t *dn, int level, uint64_t blkid, boolean_t hit,
    drain_sel_t *sel)
{
	int epbs = dn->dn_indblkshift - SPA_BLKPTRSHIFT;
	dmu_buf_impl_t *db = dbuf_hold_level(dn, level, blkid, FTAG);

	if (db == NULL) {
		sel->sel_errors++;
		return;
	}
	if (dbuf_read(db, NULL, DB_RF_CANFAIL | DB_RF_HAVESTRUCT |
	    DMU_READ_NO_PREFETCH) != 0) {
		sel->sel_errors++;
		dbuf_rele(db, FTAG);
		return;
	}

	const blkptr_t *bps = db->db.db_data;
	if (level == 1) {
		size_t u = drain_sel_unit(sel, blkid, hit);
		for (uint64_t i = 0; i < (1ULL << epbs); i++) {
			if (drain_bp_hit(&bps[i]))
				drain_sel_blk(sel, u, (blkid << epbs) + i);
		}
	} else {
		for (uint64_t i = 0; i < (1ULL << epbs); i++) {
			if (BP_IS_HOLE(&bps[i]))
				continue;
			drain_walk_ind(dn, level - 1, (blkid << epbs) + i,
			    hit || drain_bp_hit(&bps[i]), sel);
		}
	}
	dbuf_rele(db, FTAG);
}

/*
 * Select the L0 blocks of dn that have a DVA on a noalloc vdev, grouped by
 * their L1, and note which L1s are on such a vdev or under one.
 */
static void
drain_walk(dnode_t *dn, drain_sel_t *sel)
{
	rw_enter(&dn->dn_struct_rwlock, RW_READER);
	int nlevels = dn->dn_nlevels;
	int nblkptr = dn->dn_nblkptr;
	const blkptr_t *bps = dn->dn_phys->dn_blkptr;

	if (nlevels == 1) {
		size_t u = drain_sel_unit(sel, DRAIN_NO_L1, B_FALSE);
		for (int i = 0; i < nblkptr; i++) {
			if (drain_bp_hit(&bps[i]))
				drain_sel_blk(sel, u, i);
		}
	} else {
		for (int i = 0; i < nblkptr; i++) {
			if (BP_IS_HOLE(&bps[i]))
				continue;
			drain_walk_ind(dn, nlevels - 1, i,
			    drain_bp_hit(&bps[i]), sel);
		}
	}
	rw_exit(&dn->dn_struct_rwlock);
}

/*
 * Dirty block (level, blkid) of dn on its own.  In open context the caller
 * assigned tx with a hold that covers it.
 */
static int
drain_dirty_level(dnode_t *dn, int level, uint64_t blkid, dmu_tx_t *tx,
    uint64_t *sizep)
{
	rw_enter(&dn->dn_struct_rwlock, RW_READER);
	dmu_buf_impl_t *db = dbuf_hold_level(dn, level, blkid, FTAG);
	rw_exit(&dn->dn_struct_rwlock);
	if (db == NULL)
		return (SET_ERROR(ENOENT));
	dmu_buf_will_dirty(&db->db, tx);
	if (sizep != NULL)
		*sizep = db->db.db_size;
	dbuf_rele(db, FTAG);
	return (0);
}

/*
 * Open context: mark the selected L0s of dn for rewrite, a few per
 * transaction, then dirty on its own each L1 that is on a noalloc vdev (or
 * under one) and got no L0 marked.  Returns true if anything was dirtied.
 */
static boolean_t
drain_mark(objset_t *os, dnode_t *dn, drain_sel_t *sel, drain_cnt_t *cnt)
{
	boolean_t dirtied = B_FALSE;
	uint64_t bs = dn->dn_datablksz;
	int epbs = dn->dn_indblkshift - SPA_BLKPTRSHIFT;

	for (size_t u = 0; u < sel->sel_nunit; u++) {
		drain_unit_t *du = &sel->sel_unit[u];
		uint64_t marked = 0;

		for (size_t i = 0; i < du->du_count; ) {
			uint64_t *blk = &sel->sel_blk[du->du_first];
			uint64_t start = blk[i];
			size_t n = 1;

			while (i + n < du->du_count && n < DRAIN_RUN &&
			    blk[i + n] == start + n)
				n++;

			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_write_by_dnode(tx, dn, start * bs, n * bs);
			int err = dmu_tx_assign(tx, DMU_TX_WAIT);
			if (err != 0) {
				dmu_tx_abort(tx);
				cnt->dc_errors++;
				break;
			}
			uint64_t nw = 0;
			err = dmu_rewrite_by_dnode(dn, start * bs, n * bs,
			    g_drain.dr_rwflags, tx, NULL, &nw);
			dmu_tx_commit(tx);
			if (err != 0)
				cnt->dc_errors++;
			if (nw != 0) {
				cnt->dc_l0 += nw / bs;
				cnt->dc_l0_bytes += nw;
				drain_count_type(cnt, dn->dn_type, nw / bs, nw);
				marked += nw;
				dirtied = B_TRUE;
			}
			i += n;
		}

		if (du->du_hit && marked == 0 && du->du_l1 != DRAIN_NO_L1) {
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_write_by_dnode(tx, dn,
			    (du->du_l1 << epbs) * bs, 1);
			if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
				dmu_tx_abort(tx);
				cnt->dc_errors++;
				continue;
			}
			uint64_t sz = 0;
			if (drain_dirty_level(dn, 1, du->du_l1, tx, &sz) == 0) {
				cnt->dc_l1++;
				drain_count_type(cnt, dn->dn_type, 1, sz);
				dirtied = B_TRUE;
			} else {
				cnt->dc_errors++;
			}
			dmu_tx_commit(tx);
		}
	}
	return (dirtied);
}

static boolean_t
drain_spill(objset_t *os, dnode_t *dn, drain_cnt_t *cnt)
{
	boolean_t hit;

	rw_enter(&dn->dn_struct_rwlock, RW_READER);
	hit = (dn->dn_phys->dn_flags & DNODE_FLAG_SPILL_BLKPTR) &&
	    drain_bp_hit(DN_SPILL_BLKPTR(dn->dn_phys));
	rw_exit(&dn->dn_struct_rwlock);
	if (!hit)
		return (B_FALSE);

	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_spill(tx, dn->dn_object);
	if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
		dmu_tx_abort(tx);
		cnt->dc_errors++;
		return (B_FALSE);
	}
	dmu_buf_t *bonus, *spill;
	boolean_t done = B_FALSE;
	if (dmu_bonus_hold_by_dnode(dn, FTAG, &bonus,
	    DMU_READ_NO_PREFETCH) == 0) {
		if (dmu_spill_hold_existing(bonus, FTAG, &spill) == 0) {
			dmu_buf_will_dirty(spill, tx);
			drain_count_type(cnt, DMU_OT_SA, 1, spill->db_size);
			dmu_buf_rele(spill, FTAG);
			cnt->dc_spill++;
			done = B_TRUE;
		}
		dmu_buf_rele(bonus, FTAG);
	}
	if (!done)
		cnt->dc_errors++;
	dmu_tx_commit(tx);
	return (done);
}

static ssize_t
drain_find_blk(const drain_sel_t *sel, uint64_t blkid)
{
	ssize_t lo = 0, hi = (ssize_t)sel->sel_nblk - 1;

	while (lo <= hi) {
		ssize_t mid = (lo + hi) / 2;
		if (sel->sel_blk[mid] == blkid)
			return (mid);
		if (sel->sel_blk[mid] < blkid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return (-1);
}

/*
 * Open context: one filesystem or volume, owned for the whole pass.
 */
static void
drain_objset(const char *name, drain_cnt_t *cnt)
{
	objset_t *os;
	int err;

	err = dmu_objset_own(name, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os);
	if (err != 0) {
		(void) printf("DRAIN_SKIP %s: %s\n", name, err == EACCES ?
		    "encrypted, key not loaded" : strerror(err));
		cnt->dc_errors++;
		return;
	}
	cnt->dc_objsets++;

	dnode_t *mdn = DMU_META_DNODE(os);
	drain_sel_t msel = { 0 };
	drain_walk(mdn, &msel);
	boolean_t *covered = calloc(MAX(msel.sel_nblk, 1),
	    sizeof (boolean_t));
	VERIFY3P(covered, !=, NULL);
	boolean_t dirtied = B_FALSE;

	uint64_t obj = 0;
	while (dmu_object_next(os, &obj, B_FALSE, 0) == 0) {
		dnode_t *dn;

		if (dnode_hold(os, obj, FTAG, &dn) != 0) {
			cnt->dc_errors++;
			continue;
		}
		cnt->dc_objects++;

		drain_sel_t sel = { 0 };
		drain_walk(dn, &sel);
		cnt->dc_errors += sel.sel_errors;
		if (drain_mark(os, dn, &sel, cnt))
			dirtied = B_TRUE;
		drain_sel_fini(&sel);

		if (drain_spill(os, dn, cnt))
			dirtied = B_TRUE;

		ssize_t idx = drain_find_blk(&msel,
		    obj >> DNODES_PER_BLOCK_SHIFT);
		if (idx >= 0) {
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_bonus(tx, obj);
			if (dmu_tx_assign(tx, DMU_TX_WAIT) == 0) {
				dnode_setdirty(dn, tx);
				dmu_tx_commit(tx);
				if (!covered[idx]) {
					drain_count_type(cnt, DMU_OT_DNODE, 1,
					    DNODE_BLOCK_SIZE);
				}
				covered[idx] = B_TRUE;
				cnt->dc_dnode++;
				dirtied = B_TRUE;
			} else {
				dmu_tx_abort(tx);
				cnt->dc_errors++;
			}
		}
		dnode_rele(dn, FTAG);
	}

	/* Dnode blocks with no allocated dnode, and meta-dnode indirects. */
	for (size_t u = 0; u < msel.sel_nunit; u++) {
		drain_unit_t *du = &msel.sel_unit[u];
		boolean_t any = B_FALSE;

		for (size_t i = du->du_first; i < du->du_first + du->du_count;
		    i++) {
			any = B_TRUE;
			if (covered[i])
				continue;
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_space(tx, DNODE_BLOCK_SIZE);
			if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
				dmu_tx_abort(tx);
				cnt->dc_errors++;
				continue;
			}
			if (drain_dirty_level(mdn, 0, msel.sel_blk[i], tx,
			    NULL) == 0) {
				cnt->dc_mdn++;
				drain_count_type(cnt, DMU_OT_DNODE, 1,
				    DNODE_BLOCK_SIZE);
				dirtied = B_TRUE;
			} else {
				cnt->dc_errors++;
			}
			dmu_tx_commit(tx);
		}
		if (du->du_hit && !any && du->du_l1 != DRAIN_NO_L1) {
			dmu_tx_t *tx = dmu_tx_create(os);
			dmu_tx_hold_space(tx, 1ULL << mdn->dn_indblkshift);
			if (dmu_tx_assign(tx, DMU_TX_WAIT) != 0) {
				dmu_tx_abort(tx);
				cnt->dc_errors++;
				continue;
			}
			uint64_t sz = 0;
			if (drain_dirty_level(mdn, 1, du->du_l1, tx, &sz) ==
			    0) {
				cnt->dc_l1++;
				drain_count_type(cnt, DMU_OT_DNODE, 1, sz);
				dirtied = B_TRUE;
			} else {
				cnt->dc_errors++;
			}
			dmu_tx_commit(tx);
		}
	}
	free(covered);
	drain_sel_fini(&msel);

	/* Nothing else made the objset dirty: rewrite its root block. */
	dsl_dataset_t *ds = dmu_objset_ds(os);
	rrw_enter(&ds->ds_bp_rwlock, RW_READER, FTAG);
	boolean_t root_hit = drain_bp_hit(os->os_rootbp);
	rrw_exit(&ds->ds_bp_rwlock, FTAG);
	if (!dirtied && root_hit) {
		dmu_tx_t *tx = dmu_tx_create(os);
		dmu_tx_hold_space(tx, SPA_MINBLOCKSIZE);
		if (dmu_tx_assign(tx, DMU_TX_WAIT) == 0) {
			dsl_dataset_dirty(ds, tx);
			dmu_tx_commit(tx);
			cnt->dc_root++;
			drain_count_type(cnt, DMU_OT_OBJSET, 1, 0);
		} else {
			dmu_tx_abort(tx);
			cnt->dc_errors++;
		}
	}

	dmu_objset_disown(os, B_TRUE, FTAG);
}

/*
 * Syncing context: dirty the selected blocks of dn, which belongs to the
 * MOS or is one of the user/group/project used objects.
 */
static uint64_t
drain_dirty_sync(dnode_t *dn, drain_sel_t *sel, drain_cnt_t *cnt,
    dmu_tx_t *tx, uint64_t *nblocks)
{
	uint64_t bytes = 0;

	for (size_t u = 0; u < sel->sel_nunit; u++) {
		drain_unit_t *du = &sel->sel_unit[u];
		uint64_t marked = 0;

		for (size_t i = du->du_first; i < du->du_first + du->du_count;
		    i++) {
			uint64_t blkid = sel->sel_blk[i];
			dmu_buf_t *db;

			if (dnode_block_freed(dn, blkid))
				continue;
			if (dmu_buf_hold_by_dnode(dn, blkid * dn->dn_datablksz,
			    FTAG, &db, DMU_READ_NO_PREFETCH) != 0) {
				cnt->dc_errors++;
				continue;
			}
			dmu_buf_will_dirty(db, tx);
			bytes += db->db_size;
			drain_count_type(cnt, dn->dn_type, 1, db->db_size);
			dmu_buf_rele(db, FTAG);
			(*nblocks)++;
			marked++;
		}
		if (du->du_hit && marked == 0 && du->du_l1 != DRAIN_NO_L1) {
			uint64_t sz = 0;
			if (drain_dirty_level(dn, 1, du->du_l1, tx, &sz) == 0) {
				cnt->dc_l1++;
				bytes += sz;
				drain_count_type(cnt, dn->dn_type, 1, sz);
			} else {
				cnt->dc_errors++;
			}
		}
	}
	return (bytes);
}

static void
drain_mos_sync(void *arg, dmu_tx_t *tx)
{
	drain_mos_t *dm = arg;
	drain_cnt_t *cnt = dm->dm_cnt;
	objset_t *mos = dmu_tx_pool(tx)->dp_meta_objset;
	uint64_t bytes = 0;

	cnt->dc_txgs++;
	if (dm->dm_cursor == 0) {
		dnode_t *mdn = DMU_META_DNODE(mos);
		drain_sel_t sel = { 0 };

		drain_walk(mdn, &sel);
		cnt->dc_errors += sel.sel_errors;
		for (size_t u = 0; u < sel.sel_nunit; u++) {
			drain_unit_t *du = &sel.sel_unit[u];
			for (size_t i = du->du_first;
			    i < du->du_first + du->du_count; i++) {
				if (drain_dirty_level(mdn, 0, sel.sel_blk[i],
				    tx, NULL) == 0) {
					cnt->dc_mdn++;
					bytes += DNODE_BLOCK_SIZE;
					drain_count_type(cnt, DMU_OT_DNODE, 1,
					    DNODE_BLOCK_SIZE);
				} else {
					cnt->dc_errors++;
				}
			}
			if (du->du_hit && du->du_count == 0 &&
			    du->du_l1 != DRAIN_NO_L1) {
				uint64_t sz = 0;
				if (drain_dirty_level(mdn, 1, du->du_l1, tx,
				    &sz) == 0) {
					cnt->dc_l1++;
					bytes += sz;
					drain_count_type(cnt, DMU_OT_DNODE, 1,
					    sz);
				} else {
					cnt->dc_errors++;
				}
			}
		}
		drain_sel_fini(&sel);
	}

	uint64_t obj = dm->dm_cursor;
	while (bytes < g_drain.dr_mos_budget) {
		dnode_t *dn;

		if (dmu_object_next(mos, &obj, B_FALSE, 0) != 0) {
			dm->dm_done = B_TRUE;
			break;
		}
		/* Objects being freed fail to be held: skip them. */
		if (dnode_hold(mos, obj, FTAG, &dn) != 0) {
			cnt->dc_errors++;
			continue;
		}
		cnt->dc_objects++;

		drain_sel_t sel = { 0 };
		uint64_t nblocks = 0;
		drain_walk(dn, &sel);
		cnt->dc_errors += sel.sel_errors;
		uint64_t b = drain_dirty_sync(dn, &sel, cnt, tx, &nblocks);
		cnt->dc_l0 += nblocks;
		cnt->dc_l0_bytes += b;
		bytes += b;
		if (g_drain.dr_verbose && (nblocks != 0 || b != 0)) {
			char tn[64];
			drain_type_name(drain_type_index(dn->dn_type), tn,
			    sizeof (tn));
			(void) printf("  MOS_OBJ txg=%llu obj=%llu type=%s "
			    "nlevels=%u blocks=%llu bytes=%llu\n",
			    (u_longlong_t)tx->tx_txg, (u_longlong_t)obj, tn,
			    dn->dn_nlevels, (u_longlong_t)nblocks,
			    (u_longlong_t)b);
		}
		drain_sel_fini(&sel);
		dnode_rele(dn, FTAG);
	}
	dm->dm_cursor = obj;
}

/*
 * One full walk of the MOS, in as many sync tasks as the budget needs.
 */
static void
drain_mos_scan(const char *pool, drain_cnt_t *cnt)
{
	drain_mos_t dm = { 0 };

	dm.dm_cnt = cnt;
	while (!dm.dm_done) {
		int err = dsl_sync_task(pool, NULL, drain_mos_sync, &dm,
		    (int)MAX(g_drain.dr_mos_budget >> 14, 16),
		    ZFS_SPACE_CHECK_EXTRA_RESERVED);
		if (err != 0) {
			(void) printf("DRAIN_ERROR mos sync task: %d\n", err);
			cnt->dc_errors++;
			break;
		}
	}
}

typedef struct drain_bg {
	const char	*db_pool;
	volatile boolean_t db_stop;
	uint64_t	db_scans;
	drain_cnt_t	db_cnt;
} drain_bg_t;

/*
 * -c: walk the MOS over and over while the dataset pass runs, to have MOS
 * blocks dirtied in syncing context while open context reads them.
 */
static void *
drain_mos_bg(void *arg)
{
	drain_bg_t *bg = arg;

	while (!bg->db_stop) {
		drain_mos_scan(bg->db_pool, &bg->db_cnt);
		bg->db_scans++;
	}
	return (NULL);
}

static void
drain_used_sync(void *arg, dmu_tx_t *tx)
{
	drain_used_t *du = arg;
	drain_cnt_t *cnt = du->du_cnt;
	dsl_dataset_t *ds;
	objset_t *os;
	uint64_t dirtied = 0;

	if (dsl_dataset_hold(dmu_tx_pool(tx), du->du_name, FTAG, &ds) != 0) {
		cnt->dc_errors++;
		return;
	}
	if (dmu_objset_from_ds(ds, &os) != 0) {
		cnt->dc_errors++;
		dsl_dataset_rele(ds, FTAG);
		return;
	}

	/* dnode_hold() takes the hold that dbuf_create() asserts. */
	static const uint64_t used[3] = { DMU_USERUSED_OBJECT,
	    DMU_GROUPUSED_OBJECT, DMU_PROJECTUSED_OBJECT };
	for (int i = 0; i < 3; i++) {
		dnode_t *dn;
		uint64_t nblocks = 0;

		if (dnode_hold(os, used[i], FTAG, &dn) != 0)
			continue;
		cnt->dc_used_objs++;
		drain_sel_t sel = { 0 };
		drain_walk(dn, &sel);
		cnt->dc_errors += sel.sel_errors;
		(void) drain_dirty_sync(dn, &sel, cnt, tx, &nblocks);
		cnt->dc_used += nblocks;
		dirtied += nblocks;
		drain_sel_fini(&sel);
		dnode_rele(dn, FTAG);
	}

	/*
	 * Dirtying these blocks puts the dataset on dp_dirty_datasets, to be
	 * synced in pass 2, but a sync task runs after the MOS was synced in
	 * pass 1: if nothing else dirtied the MOS, spa_sync() takes the txg
	 * for a no-op and stops with the dataset still dirty (an ASSERT in
	 * debug builds).  Dirty the dataset's own MOS buffer so that the
	 * pool syncs another pass.  Workaround for the prototype only.
	 */
	if (dirtied != 0 && !g_drain.dr_no_mos_kick)
		dmu_buf_will_dirty(ds->ds_dbuf, tx);
	dsl_dataset_rele(ds, FTAG);
}

static int
drain_find_cb(const char *name, void *arg)
{
	drain_names_t *dn = arg;

	if (dn->dn_n == dn->dn_max) {
		dn->dn_max = MAX(16, dn->dn_max * 2);
		dn->dn_name = realloc(dn->dn_name,
		    dn->dn_max * sizeof (char *));
		VERIFY3P(dn->dn_name, !=, NULL);
	}
	dn->dn_name[dn->dn_n++] = strdup(name);
	return (0);
}

static uint64_t
drain_alloc(spa_t *spa, boolean_t print, const char *what)
{
	uint64_t total = 0;
	vdev_t *rvd = spa->spa_root_vdev;

	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	for (uint64_t id = 0; id < rvd->vdev_children; id++) {
		vdev_t *vd = rvd->vdev_child[id];
		vdev_stat_t vs;

		if (id >= g_drain.dr_nvdevs || !g_drain.dr_noalloc[id])
			continue;
		vdev_get_stats(vd, &vs);
		total += vs.vs_alloc;
		if (!print)
			continue;

		uint64_t sm_alloc = 0, ms_used = 0;
		for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *ms = vd->vdev_ms[m];
			if (ms->ms_sm != NULL)
				sm_alloc += space_map_allocated(ms->ms_sm);
			if (ms->ms_allocated_space != 0)
				ms_used++;
		}
		const char *cls =
		    vd->vdev_mg->mg_class == spa_special_class(spa) ?
		    "special" : vd->vdev_mg->mg_class == spa_dedup_class(spa) ?
		    "dedup" : vd->vdev_mg->mg_class == spa_normal_class(spa) ?
		    "normal" : "other";
		(void) printf("DRAIN_VDEV %s id=%llu type=%s class=%s "
		    "vs_alloc=%llu sm_alloc=%lld metaslabs_in_use=%llu/%llu "
		    "log_sms=%llu txg=%llu\n", what, (u_longlong_t)id,
		    vd->vdev_ops->vdev_op_type, cls,
		    (u_longlong_t)vs.vs_alloc, (longlong_t)sm_alloc,
		    (u_longlong_t)ms_used, (u_longlong_t)vd->vdev_ms_count,
		    (u_longlong_t)avl_numnodes(&spa->spa_sm_logs_by_txg),
		    (u_longlong_t)spa_last_synced_txg(spa));
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);
	return (total);
}

static void
drain_print_cnt(const char *what, int round, const drain_cnt_t *cnt)
{
	(void) printf("DRAIN_PASS round=%d pass=%s objsets=%llu "
	    "objects=%llu l0=%llu l0_bytes=%llu l1=%llu dnode_setdirty=%llu "
	    "dnode_blocks_direct=%llu spill=%llu root_only=%llu "
	    "used_objs=%llu used_blocks=%llu txgs=%llu errors=%llu\n",
	    round, what, (u_longlong_t)cnt->dc_objsets,
	    (u_longlong_t)cnt->dc_objects, (u_longlong_t)cnt->dc_l0,
	    (u_longlong_t)cnt->dc_l0_bytes, (u_longlong_t)cnt->dc_l1,
	    (u_longlong_t)cnt->dc_dnode, (u_longlong_t)cnt->dc_mdn,
	    (u_longlong_t)cnt->dc_spill, (u_longlong_t)cnt->dc_root,
	    (u_longlong_t)cnt->dc_used_objs, (u_longlong_t)cnt->dc_used,
	    (u_longlong_t)cnt->dc_txgs, (u_longlong_t)cnt->dc_errors);
	for (int i = 0; i < DRAIN_NTYPES; i++) {
		char tn[64];

		if (cnt->dc_type_blocks[i] == 0)
			continue;
		drain_type_name(i, tn, sizeof (tn));
		(void) printf("  DRAIN_TYPE round=%d pass=%s type=\"%s\" "
		    "blocks=%llu bytes=%llu\n", round, what, tn,
		    (u_longlong_t)cnt->dc_type_blocks[i],
		    (u_longlong_t)cnt->dc_type_bytes[i]);
	}
}

static void
drain_wait(dsl_pool_t *dp)
{
	/* Frees are deferred: let TXG_DEFER_SIZE + 2 txgs sync. */
	for (int i = 0; i < TXG_DEFER_SIZE + 2; i++)
		txg_wait_synced(dp, spa_last_synced_txg(dp->dp_spa) + 1);
}

static int
zhack_do_drain(int argc, char **argv)
{
	spa_t *spa;
	char *target;
	int c, maxrounds = 8;
	boolean_t logical = B_FALSE, no_ds = B_FALSE, no_mos = B_FALSE;
	boolean_t no_used = B_FALSE, other_acct = B_FALSE;
	boolean_t concurrent = B_FALSE, scrub = B_FALSE;

	/* Keep the output of a run that dies on an assertion. */
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	g_drain.dr_mos_budget = 16ULL << 20;
	optind = 1;
	while ((c = getopt(argc, argv, "+b:cDKlMn:OSUvz")) != -1) {
		switch (c) {
		case 'O':
			other_acct = B_TRUE;
			break;
		case 'K':
			g_drain.dr_no_mos_kick = B_TRUE;
			break;
		case 'c':
			concurrent = B_TRUE;
			break;
		case 'z':
			scrub = B_TRUE;
			break;
		case 'b':
			g_drain.dr_mos_budget = MAX(1,
			    strtoull(optarg, NULL, 0) << 10);
			break;
		case 'D':
			no_ds = B_TRUE;
			break;
		case 'l':
			logical = B_TRUE;
			break;
		case 'M':
			no_mos = B_TRUE;
			break;
		case 'n':
			maxrounds = atoi(optarg);
			break;
		case 'S':
			g_drain.dr_rwflags |= ZFS_REWRITE_SKIP_SNAPSHOT;
			break;
		case 'U':
			no_used = B_TRUE;
			break;
		case 'v':
			g_drain.dr_verbose = B_TRUE;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		(void) fprintf(stderr, "error: missing pool name\n");
		usage();
	}
	target = argv[0];

	g_no_kernel = (access(ZFS_DEV, F_OK) != 0);
	zhack_spa_open(target, B_FALSE, FTAG, &spa);
	dmu_objset_register_type(DMU_OST_ZFS, drain_get_file_info);
	if (other_acct)
		dmu_objset_register_type(DMU_OST_OTHER, drain_get_file_info);
	dsl_pool_t *dp = spa_get_dsl(spa);
	g_drain.dr_spa = spa;

	if (spa_has_checkpoint(spa))
		fatal(spa, FTAG, "pool has a checkpoint");

	if (!logical && spa_feature_is_enabled(spa,
	    SPA_FEATURE_PHYSICAL_REWRITE))
		g_drain.dr_rwflags |= ZFS_REWRITE_PHYSICAL;

	vdev_t *rvd = spa->spa_root_vdev;
	uint64_t nnoalloc = 0;
	g_drain.dr_nvdevs = rvd->vdev_children;
	g_drain.dr_noalloc = calloc(MAX(rvd->vdev_children, 1), 1);
	VERIFY3P(g_drain.dr_noalloc, !=, NULL);
	for (uint64_t id = 0; id < rvd->vdev_children; id++) {
		vdev_t *vd = rvd->vdev_child[id];
		if (vdev_is_concrete(vd) && vd->vdev_noalloc &&
		    !vd->vdev_islog) {
			g_drain.dr_noalloc[id] = 1;
			nnoalloc++;
		}
	}
	(void) printf("DRAIN_START pool=%s noalloc_vdevs=%llu rwflags=%#llx "
	    "mos_budget=%llu\n", target, (u_longlong_t)nnoalloc,
	    (u_longlong_t)g_drain.dr_rwflags,
	    (u_longlong_t)g_drain.dr_mos_budget);
	if (nnoalloc == 0) {
		spa_close(spa, FTAG);
		return (1);
	}

	uint64_t *vbefore = calloc(MAX(g_drain.dr_nvdevs, 1),
	    sizeof (uint64_t));
	VERIFY3P(vbefore, !=, NULL);
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	for (uint64_t id = 0; id < g_drain.dr_nvdevs; id++) {
		vdev_stat_t vs;
		if (!g_drain.dr_noalloc[id])
			continue;
		vdev_get_stats(rvd->vdev_child[id], &vs);
		vbefore[id] = vs.vs_alloc;
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);
	uint64_t before = drain_alloc(spa, B_TRUE, "before");
	if (scrub) {
		int err = spa_scan(spa, POOL_SCAN_SCRUB, 0);
		(void) printf("DRAIN_SCRUB start -> %d\n", err);
	}
	uint64_t prev = before, cur = before;
	int round;

	for (round = 1; round <= maxrounds; round++) {
		drain_names_t names = { 0 };
		drain_cnt_t *cnt = calloc(1, sizeof (drain_cnt_t));
		VERIFY3P(cnt, !=, NULL);

		VERIFY0(dmu_objset_find(target, drain_find_cb, &names,
		    DS_FIND_CHILDREN));

		if (!no_ds) {
			drain_bg_t bg = { 0 };
			pthread_t tid;

			bg.db_pool = target;
			if (concurrent)
				VERIFY0(pthread_create(&tid, NULL,
				    drain_mos_bg, &bg));
			for (int i = 0; i < names.dn_n; i++)
				drain_objset(names.dn_name[i], cnt);
			if (concurrent) {
				bg.db_stop = B_TRUE;
				VERIFY0(pthread_join(tid, NULL));
				(void) printf("DRAIN_BG round=%d scans=%llu\n",
				    round, (u_longlong_t)bg.db_scans);
				drain_print_cnt("mos-concurrent", round,
				    &bg.db_cnt);
			}
			txg_wait_synced(dp, 0);
			drain_print_cnt("datasets", round, cnt);
		}

		if (!no_used) {
			memset(cnt, 0, sizeof (*cnt));
			for (int i = 0; i < names.dn_n; i++) {
				drain_used_t du = { names.dn_name[i], cnt };
				int err = dsl_sync_task(target, NULL,
				    drain_used_sync, &du, 16,
				    ZFS_SPACE_CHECK_EXTRA_RESERVED);
				cnt->dc_txgs++;
				if (err != 0) {
					(void) printf("DRAIN_ERROR used sync "
					    "task %s: %d\n", names.dn_name[i],
					    err);
					cnt->dc_errors++;
				}
			}
			drain_print_cnt("used", round, cnt);
		}

		if (!no_mos) {
			memset(cnt, 0, sizeof (*cnt));
			drain_mos_scan(target, cnt);
			drain_print_cnt("mos", round, cnt);
		}

		for (int i = 0; i < names.dn_n; i++)
			free(names.dn_name[i]);
		free(names.dn_name);
		free(cnt);

		drain_wait(dp);
		cur = drain_alloc(spa, B_TRUE, "round");
		(void) printf("DRAIN_ROUND round=%d alloc=%llu\n", round,
		    (u_longlong_t)cur);
		if (cur == 0 || cur >= prev)
			break;
		prev = cur;
	}

	for (uint64_t id = 0; id < g_drain.dr_nvdevs; id++) {
		if (!g_drain.dr_noalloc[id])
			continue;
		vdev_t *vd = rvd->vdev_child[id];
		vdev_stat_t vs;
		spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
		vdev_get_stats(vd, &vs);
		spa_config_exit(spa, SCL_VDEV, FTAG);
		(void) printf("DRAIN vdev=%llu alloc_before=%llu "
		    "alloc_after=%llu rounds=%d\n", (u_longlong_t)id,
		    (u_longlong_t)vbefore[id], (u_longlong_t)vs.vs_alloc,
		    MIN(round, maxrounds));
	}

	free(vbefore);
	free(g_drain.dr_noalloc);
	spa_close(spa, FTAG);
	return (cur == 0 ? 0 : 2);
}

#define	ASHIFT_UBERBLOCK_SHIFT(ashift)	\
	MIN(MAX(ashift, UBERBLOCK_SHIFT), \
	MAX_UBERBLOCK_SHIFT)
#define	ASHIFT_UBERBLOCK_SIZE(ashift) \
	(1ULL << ASHIFT_UBERBLOCK_SHIFT(ashift))

#define	REPAIR_LABEL_STATUS_CKSUM (1 << 0)
#define	REPAIR_LABEL_STATUS_UB    (1 << 1)

static int
zhack_repair_read_label(const int fd, vdev_label_t *vl,
    const uint64_t label_offset, const int l)
{
	const int err = pread64(fd, vl, sizeof (vdev_label_t), label_offset);

	if (err == -1) {
		(void) fprintf(stderr,
		    "error: cannot read label %d: %s\n",
		    l, strerror(errno));
		return (err);
	} else if (err != sizeof (vdev_label_t)) {
		(void) fprintf(stderr,
		    "error: bad label %d read size\n", l);
		return (err);
	}

	return (0);
}

static int
zhack_repair_get_byteswap(const zio_eck_t *vdev_eck, const int l, int *byteswap)
{
	if (vdev_eck->zec_magic == ZEC_MAGIC) {
		*byteswap = B_FALSE;
	} else if (vdev_eck->zec_magic == BSWAP_64((uint64_t)ZEC_MAGIC)) {
		*byteswap = B_TRUE;
	} else {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected the nvlist checksum magic number but instead got "
		    "0x%" PRIx64 "\n",
		    l, vdev_eck->zec_magic);
		return (1);
	}
	return (0);
}

static void
zhack_repair_calc_cksum(const int byteswap, void *data, const uint64_t offset,
    const uint64_t abdsize, zio_eck_t *eck, zio_cksum_t *cksum)
{
	zio_cksum_t verifier;
	zio_cksum_t current_cksum;
	zio_checksum_info_t *ci;
	abd_t *abd;

	ZIO_SET_CHECKSUM(&verifier, offset, 0, 0, 0);

	if (byteswap)
		byteswap_uint64_array(&verifier, sizeof (zio_cksum_t));

	current_cksum = eck->zec_cksum;
	eck->zec_cksum = verifier;

	ci = &zio_checksum_table[ZIO_CHECKSUM_LABEL];
	abd = abd_get_from_buf(data, abdsize);
	ci->ci_func[byteswap](abd, abdsize, NULL, cksum);
	abd_free(abd);

	eck->zec_cksum = current_cksum;
}

static int
zhack_repair_get_ashift(nvlist_t *cfg, const int l, uint64_t *ashift)
{
	int err;
	nvlist_t *vdev_tree_cfg;

	err = nvlist_lookup_nvlist(cfg,
	    ZPOOL_CONFIG_VDEV_TREE, &vdev_tree_cfg);
	if (err) {
		(void) fprintf(stderr,
		    "error: label %d: cannot find nvlist key %s\n",
		    l, ZPOOL_CONFIG_VDEV_TREE);
		return (err);
	}

	err = nvlist_lookup_uint64(vdev_tree_cfg,
	    ZPOOL_CONFIG_ASHIFT, ashift);
	if (err) {
		(void) fprintf(stderr,
		    "error: label %d: cannot find nvlist key %s\n",
		    l, ZPOOL_CONFIG_ASHIFT);
		return (err);
	}

	if (*ashift == 0) {
		(void) fprintf(stderr,
		    "error: label %d: nvlist key %s is zero\n",
		    l, ZPOOL_CONFIG_ASHIFT);
		return (1);
	}

	return (0);
}

static int
zhack_repair_undetach(uberblock_t *ub, nvlist_t *cfg, const int l)
{
	/*
	 * Uberblock root block pointer has valid birth TXG.
	 * Copying it to the label NVlist
	 */
	if (BP_GET_LOGICAL_BIRTH(&ub->ub_rootbp) != 0) {
		const uint64_t txg = BP_GET_LOGICAL_BIRTH(&ub->ub_rootbp);
		int err;

		ub->ub_txg = txg;

		err = nvlist_remove_all(cfg, ZPOOL_CONFIG_CREATE_TXG);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: "
			    "Failed to remove pool creation TXG\n",
			    l);
			return (err);
		}

		err = nvlist_remove_all(cfg, ZPOOL_CONFIG_POOL_TXG);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: Failed to remove pool TXG to "
			    "be replaced.\n",
			    l);
			return (err);
		}

		err = nvlist_add_uint64(cfg, ZPOOL_CONFIG_POOL_TXG, txg);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d: "
			    "Failed to add pool TXG of %" PRIu64 "\n",
			    l, txg);
			return (err);
		}
	}

	return (0);
}

static boolean_t
zhack_repair_write_label(const int l, const int fd, const int byteswap,
    void *data, zio_eck_t *eck, const uint64_t offset, const uint64_t abdsize)
{
	zio_cksum_t actual_cksum;
	zhack_repair_calc_cksum(byteswap, data, offset, abdsize, eck,
	    &actual_cksum);
	zio_cksum_t expected_cksum = eck->zec_cksum;
	ssize_t err;

	if (ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum))
		return (B_FALSE);

	eck->zec_cksum = actual_cksum;

	err = pwrite64(fd, data, abdsize, offset);
	if (err == -1) {
		(void) fprintf(stderr, "error: cannot write label %d: %s\n",
		    l, strerror(errno));
		return (B_FALSE);
	} else if (err != abdsize) {
		(void) fprintf(stderr, "error: bad write size label %d\n", l);
		return (B_FALSE);
	} else {
		(void) fprintf(stderr,
		    "label %d: wrote %" PRIu64 " bytes at offset %" PRIu64 "\n",
		    l, abdsize, offset);
	}

	return (B_TRUE);
}

static void
zhack_repair_write_uberblock(vdev_label_t *vl, const int l,
    const uint64_t ashift, const int fd, const int byteswap,
    const uint64_t label_offset, uint32_t *labels_repaired)
{
	void *ub_data =
	    (char *)vl + offsetof(vdev_label_t, vl_uberblock);
	zio_eck_t *ub_eck =
	    (zio_eck_t *)
	    ((char *)(ub_data) + (ASHIFT_UBERBLOCK_SIZE(ashift))) - 1;

	if (ub_eck->zec_magic != 0) {
		(void) fprintf(stderr,
		    "error: label %d: "
		    "Expected Uberblock checksum magic number to "
		    "be 0, but got %" PRIu64 "\n",
		    l, ub_eck->zec_magic);
		(void) fprintf(stderr, "It would appear there's already "
		    "a checksum for the uberblock.\n");
		return;
	}


	ub_eck->zec_magic = byteswap ? BSWAP_64(ZEC_MAGIC) : ZEC_MAGIC;

	if (zhack_repair_write_label(l, fd, byteswap,
	    ub_data, ub_eck,
	    label_offset + offsetof(vdev_label_t, vl_uberblock),
	    ASHIFT_UBERBLOCK_SIZE(ashift)))
			labels_repaired[l] |= REPAIR_LABEL_STATUS_UB;
}

static void
zhack_repair_print_cksum(FILE *stream, const zio_cksum_t *cksum)
{
	(void) fprintf(stream,
	    "%016llx:%016llx:%016llx:%016llx",
	    (u_longlong_t)cksum->zc_word[0],
	    (u_longlong_t)cksum->zc_word[1],
	    (u_longlong_t)cksum->zc_word[2],
	    (u_longlong_t)cksum->zc_word[3]);
}

static int
zhack_repair_test_cksum(const int byteswap, void *vdev_data,
    zio_eck_t *vdev_eck, const uint64_t vdev_phys_offset, const int l)
{
	const zio_cksum_t expected_cksum = vdev_eck->zec_cksum;
	zio_cksum_t actual_cksum;
	zhack_repair_calc_cksum(byteswap, vdev_data, vdev_phys_offset,
	    VDEV_PHYS_SIZE, vdev_eck, &actual_cksum);
	const uint64_t expected_magic = byteswap ?
	    BSWAP_64(ZEC_MAGIC) : ZEC_MAGIC;
	const uint64_t actual_magic = vdev_eck->zec_magic;
	int err = 0;

	if (actual_magic != expected_magic) {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected "
		    "the nvlist checksum magic number to not be %"
		    PRIu64 " not %" PRIu64 "\n",
		    l, expected_magic, actual_magic);
		err = ECKSUM;
	}
	if (!ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum)) {
		(void) fprintf(stderr, "error: label %d: "
		    "Expected the nvlist checksum to be ", l);
		(void) zhack_repair_print_cksum(stderr,
		    &expected_cksum);
		(void) fprintf(stderr, " not ");
		zhack_repair_print_cksum(stderr, &actual_cksum);
		(void) fprintf(stderr, "\n");
		err = ECKSUM;
	}
	return (err);
}

static int
zhack_repair_unpack_cfg(vdev_label_t *vl, const int l, nvlist_t **cfg)
{
	const char *cfg_keys[] = { ZPOOL_CONFIG_VERSION,
	    ZPOOL_CONFIG_POOL_STATE, ZPOOL_CONFIG_GUID };
	int err;

	err = nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    VDEV_PHYS_SIZE - sizeof (zio_eck_t), cfg, 0);
	if (err) {
		(void) fprintf(stderr,
		    "error: cannot unpack nvlist label %d\n", l);
		return (err);
	}

	for (int i = 0; i < ARRAY_SIZE(cfg_keys); i++) {
		uint64_t val;
		err = nvlist_lookup_uint64(*cfg, cfg_keys[i], &val);
		if (err) {
			(void) fprintf(stderr,
			    "error: label %d, %d: "
			    "cannot find nvlist key %s\n",
			    l, i, cfg_keys[i]);
			return (err);
		}
	}

	return (0);
}

static void
zhack_repair_one_label(const zhack_repair_op_t op, const int fd,
    vdev_label_t *vl, const uint64_t label_offset, const int l,
    uint32_t *labels_repaired)
{
	ssize_t err;
	uberblock_t *ub = (uberblock_t *)vl->vl_uberblock;
	void *vdev_data =
	    (char *)vl + offsetof(vdev_label_t, vl_vdev_phys);
	zio_eck_t *vdev_eck =
	    (zio_eck_t *)((char *)(vdev_data) + VDEV_PHYS_SIZE) - 1;
	const uint64_t vdev_phys_offset =
	    label_offset + offsetof(vdev_label_t, vl_vdev_phys);
	nvlist_t *cfg;
	uint64_t ashift;
	int byteswap;

	err = zhack_repair_read_label(fd, vl, label_offset, l);
	if (err)
		return;

	err = zhack_repair_get_byteswap(vdev_eck, l, &byteswap);
	if (err)
		return;

	if (byteswap) {
		byteswap_uint64_array(&vdev_eck->zec_cksum,
		    sizeof (zio_cksum_t));
		vdev_eck->zec_magic = BSWAP_64(vdev_eck->zec_magic);
	}

	if ((op & ZHACK_REPAIR_OP_CKSUM) == 0 &&
	    zhack_repair_test_cksum(byteswap, vdev_data, vdev_eck,
	    vdev_phys_offset, l) != 0) {
		(void) fprintf(stderr, "It would appear checksums are "
		    "corrupted. Try zhack repair label -c <device>\n");
		return;
	}

	err = zhack_repair_unpack_cfg(vl, l, &cfg);
	if (err)
		return;

	if ((op & ZHACK_REPAIR_OP_UNDETACH) != 0) {
		char *buf;
		size_t buflen;

		if (ub->ub_txg != 0) {
			(void) fprintf(stderr,
			    "error: label %d: UB TXG of 0 expected, but got %"
			    PRIu64 "\n", l, ub->ub_txg);
			(void) fprintf(stderr, "It would appear the device was "
			    "not properly detached.\n");
			return;
		}

		err = zhack_repair_get_ashift(cfg, l, &ashift);
		if (err)
			return;

		err = zhack_repair_undetach(ub, cfg, l);
		if (err)
			return;

		buf = vl->vl_vdev_phys.vp_nvlist;
		buflen = VDEV_PHYS_SIZE - sizeof (zio_eck_t);
		if (nvlist_pack(cfg, &buf, &buflen, NV_ENCODE_XDR, 0) != 0) {
			(void) fprintf(stderr,
			    "error: label %d: Failed to pack nvlist\n", l);
			return;
		}

		zhack_repair_write_uberblock(vl,
		    l, ashift, fd, byteswap, label_offset, labels_repaired);
	}

	if (zhack_repair_write_label(l, fd, byteswap, vdev_data, vdev_eck,
	    vdev_phys_offset, VDEV_PHYS_SIZE))
			labels_repaired[l] |= REPAIR_LABEL_STATUS_CKSUM;

	fsync(fd);
}

static const char *
zhack_repair_label_status(const uint32_t label_status,
    const uint32_t to_check)
{
	return ((label_status & to_check) != 0 ? "repaired" : "skipped");
}

static int
zhack_label_repair(const zhack_repair_op_t op, const int argc, char **argv)
{
	uint32_t labels_repaired[VDEV_LABELS] = {0};
	vdev_label_t labels[VDEV_LABELS] = {{{0}}};
	struct stat64 st;
	int fd;
	off_t filesize;
	uint32_t repaired = 0;

	abd_init();

	if (argc < 1) {
		(void) fprintf(stderr, "error: missing device\n");
		usage();
	}

	if ((fd = open(argv[0], O_RDWR)) == -1)
		fatal(NULL, FTAG, "cannot open '%s': %s", argv[0],
		    strerror(errno));

	if (fstat64_blk(fd, &st) != 0)
		fatal(NULL, FTAG, "cannot stat '%s': %s", argv[0],
		    strerror(errno));

	filesize = st.st_size;
	(void) fprintf(stderr, "Calculated filesize to be %jd\n",
	    (intmax_t)filesize);

	if (filesize % sizeof (vdev_label_t) != 0)
		filesize =
		    (filesize / sizeof (vdev_label_t)) * sizeof (vdev_label_t);

	for (int l = 0; l < VDEV_LABELS; l++) {
		zhack_repair_one_label(op, fd, &labels[l],
		    vdev_label_offset(filesize, l, 0), l, labels_repaired);
	}

	close(fd);

	abd_fini();

	for (int l = 0; l < VDEV_LABELS; l++) {
		const uint32_t lr = labels_repaired[l];
		(void) printf("label %d: ", l);
		(void) printf("uberblock: %s ",
		    zhack_repair_label_status(lr, REPAIR_LABEL_STATUS_UB));
		(void) printf("checksum: %s\n",
		    zhack_repair_label_status(lr, REPAIR_LABEL_STATUS_CKSUM));
		repaired |= lr;
	}

	if (repaired > 0)
		return (0);

	return (1);
}

static int
zhack_do_label_repair(int argc, char **argv)
{
	zhack_repair_op_t op = ZHACK_REPAIR_OP_UNKNOWN;
	int c;

	optind = 1;
	while ((c = getopt(argc, argv, "+cu")) != -1) {
		switch (c) {
		case 'c':
			op |= ZHACK_REPAIR_OP_CKSUM;
			break;
		case 'u':
			op |= ZHACK_REPAIR_OP_UNDETACH;
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (op == ZHACK_REPAIR_OP_UNKNOWN)
		op = ZHACK_REPAIR_OP_CKSUM;

	return (zhack_label_repair(op, argc, argv));
}

static int
zhack_do_label(int argc, char **argv)
{
	char *subcommand;
	int err;

	argc--;
	argv++;
	if (argc == 0) {
		(void) fprintf(stderr,
		    "error: no label operation specified\n");
		usage();
	}

	subcommand = argv[0];
	if (strcmp(subcommand, "repair") == 0) {
		err = zhack_do_label_repair(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	return (err);
}

#define	MAX_NUM_PATHS 1024

int
main(int argc, char **argv)
{
	struct sigaction action;
	char *path[MAX_NUM_PATHS];
	const char *subcommand;
	int rv = 0;
	int c;

	/*
	 * Set up signal handlers, so if we crash due to bad on-disk data we
	 * can get more info. Unlike ztest, we don't bail out if we can't set
	 * up signal handlers, because zhack is very useful without them.
	 */
	action.sa_handler = sig_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if (sigaction(SIGSEGV, &action, NULL) < 0) {
		(void) fprintf(stderr, "zhack: cannot catch SIGSEGV: %s\n",
		    strerror(errno));
	}
	if (sigaction(SIGABRT, &action, NULL) < 0) {
		(void) fprintf(stderr, "zhack: cannot catch SIGABRT: %s\n",
		    strerror(errno));
	}

	g_importargs.path = path;

	dprintf_setup(&argc, argv);
	zfs_prop_init();

	while ((c = getopt(argc, argv, "+c:d:Go:")) != -1) {
		switch (c) {
		case 'c':
			g_importargs.cachefile = optarg;
			break;
		case 'd':
			assert(g_importargs.paths < MAX_NUM_PATHS);
			g_importargs.path[g_importargs.paths++] = optarg;
			break;
		case 'G':
			g_dump_dbgmsg = B_TRUE;
			break;
		case 'o':
			if (handle_tunable_option(optarg, B_FALSE) != 0)
				exit(1);
			break;
		default:
			usage();
			break;
		}
	}

	argc -= optind;
	argv += optind;
	optind = 1;

	if (argc == 0) {
		(void) fprintf(stderr, "error: no command specified\n");
		usage();
	}

	subcommand = argv[0];

	if (strcmp(subcommand, "action") == 0) {
		rv = zhack_do_action(argc, argv);
	} else if (strcmp(subcommand, "feature") == 0) {
		rv = zhack_do_feature(argc, argv);
	} else if (strcmp(subcommand, "mmp") == 0) {
		rv = zhack_do_mmp(argc, argv);
	} else if (strcmp(subcommand, "mos") == 0) {
		rv = zhack_do_mos(argc, argv);
	} else if (strcmp(subcommand, "label") == 0) {
		return (zhack_do_label(argc, argv));
	} else if (strcmp(subcommand, "metaslab") == 0) {
		rv = zhack_do_metaslab(argc, argv);
	} else if (strcmp(subcommand, "drain") == 0) {
		rv = zhack_do_drain(argc, argv);
	} else {
		(void) fprintf(stderr, "error: unknown subcommand: %s\n",
		    subcommand);
		usage();
	}

	if (!g_readonly && spa_export(g_pool, NULL, B_TRUE, B_FALSE) != 0) {
		fatal(NULL, FTAG, "pool export failed; "
		    "changes may not be committed to disk\n");
	}

	if (g_dump_dbgmsg)
		dump_debug_buffer();

	kernel_fini();

	return (rv);
}
