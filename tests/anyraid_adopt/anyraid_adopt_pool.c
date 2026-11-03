// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2026 by Tigerblue77.
 */

/*
 * Userspace adoption test of a RAID-Z vdev by AnyRAID.
 *
 * create: make a RAID-Z pool on file vdevs, write objects of several
 *   sizes, block sizes and compressibility into a dataset, snapshot it,
 *   overwrite, append, free and add objects, and export.  The expected
 *   content of every object of the head and of the snapshot is saved in a
 *   reference directory.
 *
 * verify: import the pool (with -A, with zfs_anyraid_adopt_raidz set, so
 *   that the RAID-Z vdev is opened as a legacy anyraidz vdev), print the
 *   top-level vdev, read every object back and compare it with the
 *   reference, optionally write more (-W), scrub (-S), and export.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_anyraid.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_pool.h>
#include <sys/metaslab.h>
#include <sys/zfeature.h>
#include <sys/zio_compress.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzpool.h>
#include <libzutil.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static const char *dir, *pool, *refdir;
static uint64_t width = 5, nparity = 2, ashift = 9;
static uint64_t filesize = 256ULL << 20;
static int adopt, more, scrub, readonly_import;
static char dsname[ZFS_MAX_DATASET_NAME_LEN];
static char snapname[ZFS_MAX_DATASET_NAME_LEN];

/* Objects of the first generation: block size, length, kind. */
typedef struct objspec {
	uint64_t bs;
	uint64_t len;
	int kind;	/* 0 random, 1 compressible, 2 zero runs */
} objspec_t;

static const objspec_t specs[] = {
	{ 512, 512, 0 }, { 512, 3000, 0 }, { 4096, 4096, 0 },
	{ 4096, 40000, 1 }, { 16384, (1 << 20) + 5, 0 },
	{ 131072, (3 << 20) + 333, 0 }, { 131072, 131072, 1 },
	{ 131072, 4 << 20, 2 }, { 1 << 20, (8 << 20) + 1, 0 },
	{ 1 << 20, 6 << 20, 1 }, { 16 << 20, 24 << 20, 0 },
	{ 16 << 20, 16 << 20, 1 },
};
#define	NSPECS	(sizeof (specs) / sizeof (specs[0]))
#define	NSMALL	300
#define	MAXOBJ	(NSPECS + 2 * NSMALL + 64)

typedef struct obj {
	uint64_t id;
	uint64_t len;
	uint8_t *buf;
} obj_t;

static obj_t objs[MAXOBJ];
static int nobjs;
static uint64_t rng = 12345;

static uint64_t
rnd(void)
{
	uint64_t z = (rng += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return (z ^ (z >> 31));
}

static void
fill(uint8_t *buf, uint64_t len, int kind)
{
	for (uint64_t i = 0; i < len; ) {
		uint64_t v = rnd();
		switch (kind) {
		case 0:
			for (int j = 0; j < 8 && i < len; j++, i++)
				buf[i] = (v >> (8 * j)) & 0xff;
			break;
		case 1: {
			/* runs of one of 4 letters: compresses well */
			uint64_t run = 1 + (v % 64);
			for (uint64_t j = 0; j < run && i < len; j++, i++)
				buf[i] = 'a' + ((v >> 8) & 3);
			break;
		}
		default: {
			/* 64 KiB of zeros or random */
			uint64_t run = MIN(65536, len - i);
			if (v & 1) {
				memset(buf + i, 0, run);
				i += run;
			} else {
				fill(buf + i, run, 0);
				i += run;
			}
			break;
		}
		}
	}
}

static void
die(const char *what, int err)
{
	(void) fprintf(stderr, "%s: %s\n", what, strerror(err));
	exit(1);
}

static nvlist_t *
make_root(void)
{
	nvlist_t **child = calloc(width, sizeof (nvlist_t *));
	for (uint64_t c = 0; c < width; c++) {
		static char path[MAXPATHLEN];
		(void) snprintf(path, sizeof (path), "%s/d%llu", dir,
		    (u_longlong_t)c);
		int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || ftruncate(fd, filesize) != 0)
			die(path, errno);
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, ashift);
	}
	nvlist_t *rz = fnvlist_alloc();
	fnvlist_add_string(rz, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(rz, ZPOOL_CONFIG_NPARITY, nparity);
	fnvlist_add_uint64(rz, ZPOOL_CONFIG_ASHIFT, ashift);
	fnvlist_add_nvlist_array(rz, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, width);
	nvlist_t *root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&rz, 1);
	for (uint64_t c = 0; c < width; c++)
		fnvlist_free(child[c]);
	free(child);
	fnvlist_free(rz);
	return (root);
}

static void
objset_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	(void) os, (void) arg, (void) cr, (void) tx;
}

static void
write_range(objset_t *os, obj_t *o, uint64_t off, uint64_t len)
{
	/* Bounded transactions; a 16 MiB block fits in one. */
	for (uint64_t done = 0; done < len; ) {
		uint64_t n = MIN(len - done, 16ULL << 20);
		dmu_tx_t *tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, o->id, off + done, n);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		dmu_write(os, o->id, off + done, n, o->buf + off + done, tx,
		    0);
		dmu_tx_commit(tx);
		done += n;
	}
}

static obj_t *
new_obj(objset_t *os, uint64_t bs, uint64_t len, int kind)
{
	VERIFY3S(nobjs, <, MAXOBJ);
	obj_t *o = &objs[nobjs++];
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	o->id = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 0, DMU_OT_NONE, 0,
	    tx);
	VERIFY0(dmu_object_set_blocksize(os, o->id, bs, 0, tx));
	dmu_tx_commit(tx);
	o->len = len;
	o->buf = malloc(len);
	fill(o->buf, len, kind);
	write_range(os, o, 0, len);
	return (o);
}

static void
save_refs(const char *tag)
{
	static char path[MAXPATHLEN];
	(void) snprintf(path, sizeof (path), "%s/%s.list", refdir, tag);
	FILE *list = fopen(path, "w");
	if (list == NULL)
		die(path, errno);
	for (int i = 0; i < nobjs; i++) {
		obj_t *o = &objs[i];
		if (o->buf == NULL)
			continue;
		(void) fprintf(list, "%llu %llu\n", (u_longlong_t)o->id,
		    (u_longlong_t)o->len);
		(void) snprintf(path, sizeof (path), "%s/%s.%llu", refdir, tag,
		    (u_longlong_t)o->id);
		FILE *f = fopen(path, "w");
		if (f == NULL || fwrite(o->buf, 1, o->len, f) != o->len)
			die(path, errno);
		(void) fclose(f);
	}
	(void) fclose(list);
}

static void
load_refs(const char *tag)
{
	static char path[MAXPATHLEN];
	(void) snprintf(path, sizeof (path), "%s/%s.list", refdir, tag);
	FILE *list = fopen(path, "r");
	if (list == NULL)
		die(path, errno);
	nobjs = 0;
	u_longlong_t id, len;
	while (fscanf(list, "%llu %llu", &id, &len) == 2) {
		obj_t *o = &objs[nobjs++];
		o->id = id;
		o->len = len;
		o->buf = malloc(len);
		(void) snprintf(path, sizeof (path), "%s/%s.%llu", refdir, tag,
		    id);
		FILE *f = fopen(path, "r");
		if (f == NULL || fread(o->buf, 1, len, f) != len)
			die(path, errno);
		(void) fclose(f);
	}
	(void) fclose(list);
}

static void
free_objs(void)
{
	for (int i = 0; i < nobjs; i++)
		free(objs[i].buf);
	memset(objs, 0, sizeof (objs));
	nobjs = 0;
}

/* Compare every reference object with what the dataset holds. */
static int
check_objs(const char *name, const char *tag)
{
	objset_t *os;
	int err = dmu_objset_hold(name, FTAG, &os);
	if (err != 0)
		die(name, err);
	load_refs(tag);
	uint64_t bytes = 0;
	int bad = 0;
	for (int i = 0; i < nobjs; i++) {
		obj_t *o = &objs[i];
		uint8_t *got = malloc(o->len);
		dmu_object_info_t doi;
		err = dmu_object_info(os, o->id, &doi);
		if (err == 0)
			err = dmu_read(os, o->id, 0, o->len, got,
			    DMU_READ_NO_PREFETCH);
		if (err != 0 || memcmp(got, o->buf, o->len) != 0) {
			(void) printf("  %s object %llu: %s\n", name,
			    (u_longlong_t)o->id,
			    err ? strerror(err) : "content differs");
			bad++;
		}
		bytes += o->len;
		free(got);
	}
	(void) printf("%s: %d objects, %llu bytes compared, %d bad\n", name,
	    nobjs, (u_longlong_t)bytes, bad);
	dmu_objset_rele(os, FTAG);
	return (bad);
}

static void
do_create(void)
{
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(handle_tunable_option("zfs_vdev_default_ms_shift=24",
	    B_FALSE));
	nvlist_t *root = make_root();
	nvlist_t *props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char *buf;
		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		VERIFY3S(-1, !=, asprintf(&buf, "feature@%s",
		    spa_feature_table[i].fi_uname));
		fnvlist_add_uint64(props, buf, 0);
		free(buf);
	}
	int err = spa_create(pool, root, props, NULL, NULL, NULL);
	if (err != 0)
		die("spa_create", err);
	fnvlist_free(root);
	fnvlist_free(props);

	VERIFY0(dmu_objset_create(dsname, DMU_OST_OTHER, 0, NULL,
	    objset_create_cb, NULL));
	VERIFY0(dsl_prop_set_int(dsname,
	    zfs_prop_to_name(ZFS_PROP_COMPRESSION), ZPROP_SRC_LOCAL,
	    ZIO_COMPRESS_LZ4));
	objset_t *os;
	VERIFY0(dmu_objset_own(dsname, DMU_OST_OTHER, B_FALSE, B_TRUE, FTAG,
	    &os));

	/*
	 * Spread the first generation over the vdev: after each object,
	 * write a filler of incompressible data, sized so that the pool
	 * ends up about 85% full, then free every filler.
	 */
	spa_t *spa = dmu_objset_spa(os);
	uint64_t avail = spa_get_dspace(spa) - spa_get_slop_space(spa);
	uint64_t gap = avail * 85 / 100 / (NSPECS + NSMALL);
	uint64_t fillers[NSPECS + NSMALL];
	int nfill = 0;
	obj_t filler = { 0 };
	filler.buf = malloc(gap);
	for (int i = 0; i < NSPECS + NSMALL; i++) {
		if (i < NSPECS) {
			(void) new_obj(os, specs[i].bs, specs[i].len,
			    specs[i].kind);
		} else {
			uint64_t bs = 512ULL << (rnd() % 9);
			(void) new_obj(os, bs, 1 + rnd() % (3 * bs),
			    rnd() % 3);
		}
		uint64_t len = MAX(gap, objs[nobjs - 1].len) -
		    objs[nobjs - 1].len;
		if (len == 0)
			continue;
		dmu_tx_t *tx = dmu_tx_create(os);
		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		filler.id = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, 131072,
		    DMU_OT_NONE, 0, tx);
		dmu_tx_commit(tx);
		fill(filler.buf, len, 0);
		write_range(os, &filler, 0, len);
		fillers[nfill++] = filler.id;
	}
	txg_wait_synced(dmu_objset_pool(os), 0);
	(void) printf("filled: alloc %llu of %llu\n",
	    (u_longlong_t)metaslab_class_get_alloc(spa_normal_class(spa)),
	    (u_longlong_t)metaslab_class_get_space(spa_normal_class(spa)));
	for (int i = 0; i < nfill; i++)
		VERIFY0(dmu_free_long_object(os, fillers[i]));
	free(filler.buf);
	txg_wait_synced(dmu_objset_pool(os), 0);
	VERIFY0(dmu_objset_snapshot_one(dsname, "snap1"));
	save_refs("snap1");

	/* Overwrite part of every third object, append to some, free some. */
	for (int i = 0; i < nobjs; i++) {
		obj_t *o = &objs[i];
		if (i % 3 == 0) {
			uint64_t off = rnd() % o->len;
			uint64_t len = 1 + rnd() % (o->len - off);
			fill(o->buf + off, len, 0);
			write_range(os, o, off, len);
		} else if (i % 7 == 1) {
			uint64_t add = 1 + rnd() % 70000;
			o->buf = realloc(o->buf, o->len + add);
			fill(o->buf + o->len, add, 1);
			write_range(os, o, o->len, add);
			o->len += add;
		} else if (i % 11 == 2) {
			VERIFY0(dmu_free_long_object(os, o->id));
			free(o->buf);
			o->buf = NULL;
		}
	}
	for (int i = 0; i < NSMALL; i++) {
		uint64_t bs = 512ULL << (rnd() % 9);
		(void) new_obj(os, bs, 1 + rnd() % (3 * bs), rnd() % 3);
	}
	txg_wait_synced(dmu_objset_pool(os), 0);
	save_refs("head");
	free_objs();
	dmu_objset_disown(os, B_TRUE, FTAG);

	VERIFY0(spa_open(pool, &spa, FTAG));
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	(void) printf("created %s: %s, %llu children, ashift %llu, asize "
	    "0x%llx, ms_shift %llu, %llu metaslabs, alloc %llu\n", pool,
	    tvd->vdev_ops->vdev_op_type, (u_longlong_t)tvd->vdev_children,
	    (u_longlong_t)tvd->vdev_ashift, (u_longlong_t)tvd->vdev_asize,
	    (u_longlong_t)tvd->vdev_ms_shift,
	    (u_longlong_t)tvd->vdev_ms_count,
	    (u_longlong_t)metaslab_class_get_alloc(spa_normal_class(spa)));
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
}

static int
activate_check(void *arg, dmu_tx_t *tx)
{
	(void) arg, (void) tx;
	return (0);
}

static void
activate_sync(void *arg, dmu_tx_t *tx)
{
	spa_t *spa = arg;
	if (!spa_feature_is_active(spa, SPA_FEATURE_ANYRAID))
		spa_feature_incr(spa, SPA_FEATURE_ANYRAID, tx);
}

static void
print_vdev(spa_t *spa)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	(void) printf("top-level vdev: %s, %llu children, nparity %llu, "
	    "ashift %llu, asize 0x%llx, ms_shift %llu, %llu metaslabs",
	    tvd->vdev_ops->vdev_op_type, (u_longlong_t)tvd->vdev_children,
	    (u_longlong_t)vdev_get_nparity(tvd),
	    (u_longlong_t)tvd->vdev_ashift, (u_longlong_t)tvd->vdev_asize,
	    (u_longlong_t)tvd->vdev_ms_shift,
	    (u_longlong_t)tvd->vdev_ms_count);
	if (vdev_is_anyraid(tvd)) {
		vdev_anyraid_t *va = tvd->vdev_tsd;
		(void) printf(", anyraid tile 0x%llx per child, %lu tiles, "
		    "legacy %d", (u_longlong_t)va->vd_tile_size,
		    avl_numnodes(&va->vd_tile_map), va->vd_legacy);
	}
	(void) printf("\n");
}

static int
print_errors(spa_t *spa)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	int errs = 0;
	for (uint64_t c = 0; c < tvd->vdev_children; c++) {
		vdev_stat_t *vs = &tvd->vdev_child[c]->vdev_stat;
		(void) printf("  child %llu: read %llu write %llu cksum %llu "
		    "self-healed %llu\n", (u_longlong_t)c,
		    (u_longlong_t)vs->vs_read_errors,
		    (u_longlong_t)vs->vs_write_errors,
		    (u_longlong_t)vs->vs_checksum_errors,
		    (u_longlong_t)vs->vs_self_healed);
		errs += vs->vs_read_errors + vs->vs_write_errors +
		    vs->vs_checksum_errors + (vs->vs_self_healed != 0);
	}
	vdev_stat_t *vs = &tvd->vdev_stat;
	(void) printf("  top: read %llu write %llu cksum %llu; errlog %llu\n",
	    (u_longlong_t)vs->vs_read_errors,
	    (u_longlong_t)vs->vs_write_errors,
	    (u_longlong_t)vs->vs_checksum_errors,
	    (u_longlong_t)spa_approx_errlog_size(spa));
	return (errs + vs->vs_read_errors + vs->vs_write_errors +
	    vs->vs_checksum_errors + spa_approx_errlog_size(spa));
}

static int
do_verify(void)
{
	kernel_init(readonly_import ? SPA_MODE_READ :
	    (SPA_MODE_READ | SPA_MODE_WRITE));
	if (adopt)
		VERIFY0(handle_tunable_option("zfs_anyraid_adopt_raidz=1",
		    B_FALSE));

	importargs_t args = { 0 };
	nvlist_t *cfg = NULL;
	char *searchdirs[1] = { (char *)dir };
	args.paths = 1;
	args.path = searchdirs;
	args.can_be_active = B_TRUE;
	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	VERIFY0(zpool_find_config(&lpch, pool, &cfg, &args));
	nvlist_t *props = NULL;
	if (readonly_import) {
		props = fnvlist_alloc();
		fnvlist_add_uint64(props,
		    zpool_prop_to_name(ZPOOL_PROP_READONLY), 1);
	}
	int err = spa_import((char *)pool, cfg, props, ZFS_IMPORT_NORMAL);
	fnvlist_free(cfg);
	if (props)
		fnvlist_free(props);
	if (err != 0)
		die("spa_import", err);

	spa_t *spa;
	VERIFY0(spa_open(pool, &spa, FTAG));
	print_vdev(spa);
	int bad = check_objs(snapname, "snap1");
	free_objs();
	bad += check_objs(dsname, "head");

	if (adopt && !readonly_import &&
	    spa_feature_is_enabled(spa, SPA_FEATURE_ANYRAID) &&
	    !spa_feature_is_active(spa, SPA_FEATURE_ANYRAID)) {
		VERIFY0(dsl_sync_task(pool, activate_check, activate_sync, spa,
		    0, ZFS_SPACE_CHECK_NONE));
		(void) printf("activated feature anyraid\n");
	}

	if (more && !readonly_import) {
		/* objs holds the head references: add to them. */
		objset_t *os;
		VERIFY0(dmu_objset_own(dsname, DMU_OST_OTHER, B_FALSE, B_TRUE,
		    FTAG, &os));
		int n = nobjs;
		for (int i = 0; i < n; i += 5) {
			obj_t *o = &objs[i];
			uint64_t off = rnd() % o->len;
			uint64_t len = 1 + rnd() % (o->len - off);
			fill(o->buf + off, len, 0);
			write_range(os, o, off, len);
		}
		for (int i = 0; i < 40; i++) {
			uint64_t bs = 512ULL << (rnd() % 16);
			bs = MIN(bs, 16ULL << 20);
			(void) new_obj(os, bs, 1 + rnd() % (2 * bs), rnd() % 3);
		}
		txg_wait_synced(dmu_objset_pool(os), 0);
		dmu_objset_disown(os, B_TRUE, FTAG);
		save_refs("head");
		(void) printf("wrote more: %d objects now in head\n", nobjs);
	}
	free_objs();

	if (scrub && !readonly_import) {
		dsl_pool_t *dp = spa_get_dsl(spa);
		VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
		while (dsl_scan_scrubbing(dp))
			txg_wait_synced(dp, 0);
		dsl_scan_phys_t *scn = &dp->dp_scan->scn_phys;
		(void) printf("scrub: state %llu, examined %llu, errors %llu\n",
		    (u_longlong_t)scn->scn_state,
		    (u_longlong_t)scn->scn_examined,
		    (u_longlong_t)scn->scn_errors);
		bad += scn->scn_errors;
	}
	bad += print_errors(spa);
	print_vdev(spa);
	spa_close(spa, FTAG);
	if (getenv("ADOPT_DBGMSG") != NULL)
		zfs_dbgmsg_print(STDOUT_FILENO, "anyraid_adopt_pool");
	/* A read-only pool cannot be exported; kernel_fini() unloads it. */
	if (!readonly_import)
		VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	(void) printf("%s\n", bad == 0 ? "VERIFY OK" : "VERIFY FAILED");
	return (bad == 0 ? 0 : 1);
}

static void
usage(void)
{
	(void) fprintf(stderr,
	    "usage: anyraid_adopt_pool create -d dir -p pool -r refdir "
	    "[-w width] [-P parity] [-a ashift] [-s filesize]\n"
	    "       anyraid_adopt_pool verify -d dir -p pool -r refdir "
	    "[-A] [-W] [-S] [-R] [-o tunable=value]...\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	const char *cmd = argv[1];
	argc--;
	argv++;
	int c;
	while ((c = getopt(argc, argv, "d:p:r:w:P:a:s:AWSRo:")) != -1) {
		switch (c) {
		case 'd': dir = optarg; break;
		case 'p': pool = optarg; break;
		case 'r': refdir = optarg; break;
		case 'w': width = strtoull(optarg, NULL, 0); break;
		case 'P': nparity = strtoull(optarg, NULL, 0); break;
		case 'a': ashift = strtoull(optarg, NULL, 0); break;
		case 's': filesize = strtoull(optarg, NULL, 0); break;
		case 'A': adopt = 1; break;
		case 'W': more = 1; break;
		case 'S': scrub = 1; break;
		case 'R': readonly_import = 1; break;
		case 'o':
			if (handle_tunable_option(optarg, B_FALSE) != 0)
				exit(2);
			break;
		default: usage();
		}
	}
	if (dir == NULL || pool == NULL || refdir == NULL)
		usage();
	(void) snprintf(dsname, sizeof (dsname), "%s/ds", pool);
	(void) snprintf(snapname, sizeof (snapname), "%s/ds@snap1", pool);
	if (strcmp(cmd, "create") == 0) {
		do_create();
		return (0);
	}
	if (strcmp(cmd, "verify") == 0)
		return (do_verify());
	usage();
	return (2);
}
