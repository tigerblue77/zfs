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
 * Spike (throwaway, not part of any tree): the module's test-only mover
 * (module/zfs/vdev_raidz_mover.c), run in userspace through the same entry
 * points as the kernel's raidz_spike_mover parameter, on an exported pool
 * of file vdevs, such as the common fixtures of contrib/raidz-parity-fixtures.
 * Its results are the reference a kernel run of the same input is compared
 * with.
 *
 *   kmover_user <dir> <pool> <rw|ro> <command> [<command> ...]
 *
 * <dir> holds the pool's members, found by scanning it; no cache file is
 * written outside it.  Each command is one argument: "rewrite-feature"
 * enables feature@physical_rewrite, as "zpool set
 * feature@physical_rewrite=enabled" does; any other is written to the mover
 * with the pool name inserted after its first word ("epoch", "move hybrid",
 * "census") and its result line printed.  A rw pool is exported at the end.
 * Exit status: 0 if every command returned rc=0, 1 if one did not, 2 usage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zfeature.h>
#include <libzutil.h>
#include <libzpool.h>

static int
rw_feature_check(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	if (spa_feature_is_enabled(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE))
		return (SET_ERROR(EEXIST));
	return (0);
}

static void
rw_feature_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_feature_enable(dmu_tx_pool(tx)->dp_spa,
	    SPA_FEATURE_PHYSICAL_REWRITE, tx);
}

static int
run(const char *pool, const char *cmd)
{
	char req[1024], res[4096];
	const char *sp = strchr(cmd, ' ');
	size_t w = sp == NULL ? strlen(cmd) : (size_t)(sp - cmd);
	int err;

	(void) snprintf(req, sizeof (req), "%.*s %s%s", (int)w, cmd, pool,
	    sp == NULL ? "" : sp);
	if ((err = vdev_raidz_spike_mover_start(req)) != 0) {
		(void) printf("START %s: error %d\n", req, err);
		return (1);
	}
	for (;;) {
		(void) vdev_raidz_spike_mover_status(res, sizeof (res));
		if (strncmp(res, "running ", 8) != 0)
			break;
		(void) usleep(100000);
	}
	(void) printf("RESULT %s", res);
	return (strstr(res, " rc=0\n") != NULL ? 0 : 1);
}

int
main(int argc, char **argv)
{
	importargs_t ia = { 0 };
	char *paths[1];
	char cache[4096];
	nvlist_t *config = NULL, *props = NULL;
	libpc_handle_t lpch = {
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	boolean_t ro;
	int rc = 0;

	if (argc < 5 || (strcmp(argv[3], "rw") != 0 &&
	    strcmp(argv[3], "ro") != 0)) {
		(void) fprintf(stderr, "usage: %s <dir> <pool> <rw|ro> "
		    "<command> [<command> ...]\n", argv[0]);
		return (2);
	}
	ro = strcmp(argv[3], "ro") == 0;
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(ro ? SPA_MODE_READ : SPA_MODE_READ | SPA_MODE_WRITE);
	paths[0] = argv[1];
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&lpch, argv[2], &config, &ia));
	if (!ro) {
		props = fnvlist_alloc();
		fnvlist_add_string(props, "cachefile", "none");
	}
	VERIFY0(spa_import(argv[2], config, props, ro ?
	    ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	if (props != NULL)
		fnvlist_free(props);
	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "rewrite-feature") == 0) {
			int err = dsl_sync_task(argv[2], rw_feature_check,
			    rw_feature_sync, NULL, 0, ZFS_SPACE_CHECK_NONE);

			(void) printf("RESULT rewrite-feature rc=%d\n",
			    err == EEXIST ? 0 : err);
			if (err != 0 && err != EEXIST)
				rc = 1;
			continue;
		}
		rc |= run(argv[2], argv[i]);
	}
	/* As the generator: a read-only import is left to kernel_fini(). */
	if (!ro)
		VERIFY0(spa_export(argv[2], NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (rc);
}
