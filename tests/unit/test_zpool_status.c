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
 * Copyright (c) 2026, Tigerblue77.
 */

#include <stdlib.h>
#include <string.h>
#include <libzfs.h>

#include "unit.h"

/* ========== */

/*
 * zpool_main.c is built into this test, so that its static functions can be
 * called, and its zpool_vdev_name() and free() calls go through the two
 * functions below, which keep the names it has not freed yet.
 */

#define	MAX_NAMES	32

static char *vdev_names[MAX_NAMES];

static char *
tracked_vdev_name(libzfs_handle_t *hdl, zpool_handle_t *zhp, nvlist_t *nv,
    int name_flags)
{
	char *name = zpool_vdev_name(hdl, zhp, nv, name_flags);
	int i = 0;

	while (i < MAX_NAMES && vdev_names[i] != NULL)
		i++;
	unit_lt(i, MAX_NAMES);
	vdev_names[i] = name;
	return (name);
}

static void
tracked_free(void *ptr)
{
	for (int i = 0; ptr != NULL && i < MAX_NAMES; i++) {
		if (vdev_names[i] == ptr)
			vdev_names[i] = NULL;
	}
	free(ptr);
}

static int
vdev_names_held(void)
{
	int held = 0;

	for (int i = 0; i < MAX_NAMES; i++) {
		if (vdev_names[i] != NULL)
			held++;
	}
	return (held);
}

int zpool_main(int argc, char **argv);

#define	zpool_vdev_name(hdl, zhp, nv, flags)	\
	tracked_vdev_name(hdl, zhp, nv, flags)
#define	free(ptr)	tracked_free(ptr)
#define	main		zpool_main
#include "zpool_main.c"
#undef	main
#undef	free
#undef	zpool_vdev_name

/* A pool handle made by hand, as zpool_open() needs /dev/zfs. */
#include "libzfs_impl.h"

/* ========== */

/* A vdev with healthy, zeroed stats, which takes over its children. */
static nvlist_t *
make_vdev(const char *type, const char *path, uint64_t id, uint64_t guid,
    nvlist_t **child, uint_t children)
{
	nvlist_t *nv = fnvlist_alloc();
	vdev_stat_t vs = { 0 };

	vs.vs_state = VDEV_STATE_HEALTHY;
	fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, type);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_ID, id);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_GUID, guid);
	if (path != NULL)
		fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64_array(nv, ZPOOL_CONFIG_VDEV_STATS,
	    (uint64_t *)&vs, sizeof (vs) / sizeof (uint64_t));
	if (children != 0) {
		fnvlist_add_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t * const *)child, children);
	}
	for (uint_t c = 0; c < children; c++)
		fnvlist_free(child[c]);
	return (nv);
}

/*
 * A pool with a mirror and a file as normal vdevs, then a log, a special
 * and a dedup vdev, and a special vdev that has been removed.
 */
static nvlist_t *
make_config(void)
{
	nvlist_t *config = fnvlist_alloc();
	nvlist_t *leaf[2];
	nvlist_t *top[6];
	nvlist_t *root;

	leaf[0] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/a", 0, 11, NULL, 0);
	leaf[1] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/b", 1, 12, NULL, 0);
	top[0] = make_vdev(VDEV_TYPE_MIRROR, NULL, 0, 10, leaf, 2);
	top[1] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/c", 1, 20, NULL, 0);
	top[2] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/log", 2, 30, NULL, 0);
	fnvlist_add_uint64(top[2], ZPOOL_CONFIG_IS_LOG, 1);
	top[3] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/special", 3, 40, NULL, 0);
	fnvlist_add_string(top[3], ZPOOL_CONFIG_ALLOCATION_BIAS,
	    VDEV_ALLOC_BIAS_SPECIAL);
	top[4] = make_vdev(VDEV_TYPE_FILE, "/var/tmp/dedup", 4, 50, NULL, 0);
	fnvlist_add_string(top[4], ZPOOL_CONFIG_ALLOCATION_BIAS,
	    VDEV_ALLOC_BIAS_DEDUP);
	top[5] = make_vdev(VDEV_TYPE_INDIRECT, NULL, 5, 60, NULL, 0);
	fnvlist_add_string(top[5], ZPOOL_CONFIG_ALLOCATION_BIAS,
	    VDEV_ALLOC_BIAS_SPECIAL);
	root = make_vdev(VDEV_TYPE_ROOT, NULL, 0, 1, top, 6);

	fnvlist_add_nvlist(config, ZPOOL_CONFIG_VDEV_TREE, root);
	fnvlist_free(root);
	return (config);
}

/* The GUID of the one vdev of each class that make_config() lists. */
static const char *
class_guid(const char *class)
{
	if (strcmp(class, VDEV_ALLOC_BIAS_DEDUP) == 0)
		return ("50");
	if (strcmp(class, VDEV_ALLOC_BIAS_SPECIAL) == 0)
		return ("40");
	return ("30");
}

/* The only pair of an nvlist. */
static nvpair_t *
only_pair(nvlist_t *nvl)
{
	nvpair_t *pair = nvlist_next_nvpair(nvl, NULL);

	unit_notnull(pair);
	unit_null(nvlist_next_nvpair(nvl, pair));
	return (pair);
}

/*
 * For zpool status -j, class_vdevs_nvlist() lists the top-level vdevs of
 * one allocation class, under the class or, with --json-flat-vdevs, in
 * the list itself, and skips every other top-level vdev.  Each vdev name
 * it gets, whether it lists the vdev or skips it, has to be freed.
 */
static MunitResult
test_class_vdevs(const MunitParameter params[], void *data)
{
	(void) data;
	const char *class = munit_parameters_get(params, "class");
	boolean_t flat = strcmp(munit_parameters_get(params, "layout"),
	    "flat") == 0;
	struct libzfs_handle hdl = { 0 };
	struct zpool_handle zh = { 0 };
	status_cbdata_t cb = { 0 };
	nvlist_t *config = make_config();
	nvlist_t *item = fnvlist_alloc();
	nvlist_t *vdev;

	hdl.libzfs_fd = -1;
	g_zfs = &hdl;
	zh.zpool_hdl = &hdl;
	(void) strlcpy(zh.zpool_name, "testpool", sizeof (zh.zpool_name));
	zh.zpool_config = config;
	cb.cb_json = B_TRUE;
	cb.cb_flat_vdevs = flat;

	class_vdevs_nvlist(&zh, &cb,
	    fnvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE), class, item);

	if (flat) {
		vdev = fnvpair_value_nvlist(only_pair(item));
	} else {
		unit_str_eq(nvpair_name(only_pair(item)), class);
		vdev = fnvpair_value_nvlist(only_pair(
		    fnvlist_lookup_nvlist(item, class)));
	}
	unit_str_eq(fnvlist_lookup_string(vdev, "guid"), class_guid(class));
	unit_eq(vdev_names_held(), 0);

	fnvlist_free(item);
	fnvlist_free(config);
	g_zfs = NULL;
	return (MUNIT_OK);
}

static const MunitParameterEnum class_vdevs_params[] = {
	UNIT_PARAM("class", VDEV_ALLOC_BIAS_DEDUP, VDEV_ALLOC_BIAS_SPECIAL,
	    VDEV_ALLOC_CLASS_LOGS),
	UNIT_PARAM("layout", "nested", "flat"),
	{ NULL, NULL },
};

/* ========== */

static const MunitTest zpool_status_tests[] = {
	UNIT_TEST("class_vdevs", test_class_vdevs, class_vdevs_params),
	{ 0 },
};

static const MunitSuite zpool_status_test_suite = {
	"zpool_status.",
	zpool_status_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE,
};

int
main(int argc, char **argv)
{
	return (munit_suite_main(&zpool_status_test_suite, NULL, argc, argv));
}
