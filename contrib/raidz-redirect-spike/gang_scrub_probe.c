// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike, fixes (throwaway): does a sorted scrub check the copies of a gang
 * header other than its DVA 0?  dva2scrub found that it does not, moved or
 * not.  This program uses nothing of the spike, so that it builds against
 * any libzpool of this era (build-tool.sh with that tree) and shows whether
 * the base and upstream behave the same.
 *
 *   gang_scrub_probe <dir> <ashift> setup <rzmirror|single> [c2gang]
 *   gang_scrub_probe <dir> <ashift> damage <others|first>
 *   gang_scrub_probe <dir> <ashift> scrub <sorted|legacy>
 *   gang_scrub_probe <dir> <ashift> check
 *
 * Each step is its own process; the pool is exported between them.
 *
 * setup: rzmirror is a five-wide raidz1 of files v0..v4 with a mirror of m0
 * and m1 added beside it before any data is written (crash_probe's dva2
 * layout, without the parity-2 epoch); single is one file s0.  Written: 32
 * one-block objects with copies=2 (plain-*), 8 with copies=1 and every
 * block of 100K or more ganged (gang-*, whose headers get two copies) and,
 * with c2gang, 8 more with copies=2 (gang2-*: three header copies, two per
 * child).  Each DVA on the mirror (rzmirror) or on s0 (single) is a
 * target, listed in <dir>/targets; on rzmirror it is damaged on m0, and m1
 * keeps the good bytes.  Gang children are found by parsing the header as
 * it lies on m1 or s0.
 *
 * damage: invert the first sector of every target of a block with several
 * DVAs, at a DVA other than DVA 0 (others) or at DVA 0 (first), and save
 * its bytes in <dir>/orig.
 *
 * scrub: import, scrub (legacy sets zfs_scan_legacy=1), wait, and print
 * the scan stats and the leaves' checksum errors.  With GSP_DBGMSG set,
 * the debug message buffer is printed at the end.
 *
 * check: whether each damaged sector holds its saved bytes again, with the
 * counts by kind (plain, ghdr: gang header, gchild: gang child); exits 1
 * if any does not.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_scan.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"gsp"
#define	WIDTH		5
#define	FILE_SIZE	(256ULL << 20)
#define	BLKSZ		(128ULL << 10)
#define	PATH_BUF	512
#define	NPLAIN		32
#define	NGANG		8
#define	MAXT		512
#define	SCRUB_TXGS	400

extern uint64_t metaslab_force_ganging;
extern uint_t metaslab_force_ganging_pct;

typedef struct target {
	char		t_kind[8];	/* plain, ghdr, gchild */
	char		t_name[16];	/* object name */
	int		t_dva;		/* DVA index in its block pointer */
	int		t_ndvas;
	uint64_t	t_vdev;		/* top-level vdev */
	uint64_t	t_off;		/* DVA offset */
	char		t_bad[8];	/* leaf damaged: m0 or s0 */
	char		t_ref[8];	/* leaf with the good copy: m1 or s0 */
} target_t;

static const char *dir;
static uint64_t ashift;
static target_t tg[MAXT];
static int ntg;

static void
fill_pattern(uint64_t *buf, size_t size, uint64_t seed)
{
	uint64_t x = 0x9e3779b97f4a7c15ULL ^ (seed * 0x2545f4914f6cdd1dULL);

	for (size_t i = 0; i < size / sizeof (uint64_t); i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = x;
	}
}

static void
leaf_path(char *buf, size_t len, const char *leaf)
{
	(void) snprintf(buf, len, "%s/%s", dir, leaf);
}

static nvlist_t *
file_nv(const char *leaf)
{
	char path[PATH_BUF];
	nvlist_t *nv;
	int fd;

	leaf_path(path, sizeof (path), leaf);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, !=, -1);
	VERIFY0(ftruncate(fd, FILE_SIZE));
	(void) close(fd);
	nv = fnvlist_alloc();
	fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_ASHIFT, ashift);
	return (nv);
}

static nvlist_t *
root_nv(nvlist_t *top)
{
	nvlist_t *root = fnvlist_alloc();

	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&top, 1);
	return (root);
}

static void
create_pool(boolean_t single)
{
	nvlist_t *child[WIDTH], *top, *root, *props;

	if (single) {
		top = file_nv("s0");
	} else {
		for (int c = 0; c < WIDTH; c++) {
			char leaf[8];

			(void) snprintf(leaf, sizeof (leaf), "v%d", c);
			child[c] = file_nv(leaf);
		}
		top = fnvlist_alloc();
		fnvlist_add_string(top, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
		fnvlist_add_uint64(top, ZPOOL_CONFIG_NPARITY, 1);
		fnvlist_add_uint64(top, ZPOOL_CONFIG_IS_LOG, 0);
		fnvlist_add_nvlist_array(top, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t * const *)child, WIDTH);
		for (int c = 0; c < WIDTH; c++)
			fnvlist_free(child[c]);
	}
	root = root_nv(top);
	props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char buf[256];

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		(void) snprintf(buf, sizeof (buf), "feature@%s",
		    spa_feature_table[i].fi_uname);
		fnvlist_add_uint64(props, buf, 0);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(top);
}

static void
add_mirror(spa_t *spa)
{
	nvlist_t *child[2], *mirror, *root;

	child[0] = file_nv("m0");
	child[1] = file_nv("m1");
	mirror = fnvlist_alloc();
	fnvlist_add_string(mirror, ZPOOL_CONFIG_TYPE, VDEV_TYPE_MIRROR);
	fnvlist_add_uint64(mirror, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(mirror, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, 2);
	root = root_nv(mirror);
	VERIFY0(spa_vdev_add(spa, root, B_FALSE));
	fnvlist_free(root);
	fnvlist_free(mirror);
	fnvlist_free(child[0]);
	fnvlist_free(child[1]);
}

static spa_t *
import_pool(void)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL;
	spa_t *spa;
	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	memset(&ia, 0, sizeof (ia));
	paths[0] = (char *)dir;
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	VERIFY0(zpool_find_config(&lpch, POOL, &config, &ia));
	VERIFY0(spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	return (spa);
}

static void
export_pool(spa_t *spa)
{
	spa_close(spa, FTAG);
	VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
}

static uint64_t
put_object(objset_t *os, uint64_t seed)
{
	uint64_t *buf = malloc(BLKSZ);
	dmu_tx_t *tx;
	uint64_t obj;

	fill_pattern(buf, BLKSZ, seed);
	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, BLKSZ);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	obj = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, BLKSZ,
	    DMU_OT_NONE, 0, tx);
	dmu_write(os, obj, 0, BLKSZ, buf, tx, DMU_READ_NO_PREFETCH);
	dmu_tx_commit(tx);
	free(buf);
	return (obj);
}

static blkptr_t
object_bp(objset_t *os, uint64_t obj)
{
	dmu_buf_t *db;
	blkptr_t bp;

	VERIFY0(dmu_buf_hold(os, obj, 0, FTAG, &db, DMU_READ_NO_PREFETCH));
	bp = *dmu_buf_get_blkptr(db);
	dmu_buf_rele(db, FTAG);
	return (bp);
}

/* The leaf to damage and the leaf with the good bytes, or B_FALSE. */
static boolean_t
dva_leaves(spa_t *spa, const dva_t *dva, const char **bad, const char **ref)
{
	vdev_t *tvd = vdev_lookup_top(spa, DVA_GET_VDEV(dva));

	if (tvd->vdev_ops == &vdev_mirror_ops) {
		*bad = "m0";
		*ref = "m1";
		return (B_TRUE);
	}
	if (tvd->vdev_ops == &vdev_file_ops) {
		*bad = "s0";
		*ref = "s0";
		return (B_TRUE);
	}
	return (B_FALSE);
}

static void
add_targets(spa_t *spa, const char *kind, const char *name,
    const blkptr_t *bp)
{
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		const dva_t *dva = &bp->blk_dva[d];
		const char *bad, *ref;
		target_t *t;

		if (!dva_leaves(spa, dva, &bad, &ref))
			continue;
		VERIFY3S(ntg, <, MAXT);
		t = &tg[ntg++];
		(void) strlcpy(t->t_kind, kind, sizeof (t->t_kind));
		(void) strlcpy(t->t_name, name, sizeof (t->t_name));
		t->t_dva = d;
		t->t_ndvas = BP_GET_NDVAS(bp);
		t->t_vdev = DVA_GET_VDEV(dva);
		t->t_off = DVA_GET_OFFSET(dva);
		(void) strlcpy(t->t_bad, bad, sizeof (t->t_bad));
		(void) strlcpy(t->t_ref, ref, sizeof (t->t_ref));
	}
}

static void
print_bp(const char *name, const blkptr_t *bp)
{
	(void) printf("INFO %s:%s %d DVAs psize=%llu birth=%llu",
	    name, BP_IS_GANG(bp) ? " gang header," : "", BP_GET_NDVAS(bp),
	    (u_longlong_t)BP_GET_PSIZE(bp),
	    (u_longlong_t)BP_GET_PHYSICAL_BIRTH(bp));
	for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
		(void) printf(" [%d]=%llu:%llx:%llx", d,
		    (u_longlong_t)DVA_GET_VDEV(&bp->blk_dva[d]),
		    (u_longlong_t)DVA_GET_OFFSET(&bp->blk_dva[d]),
		    (u_longlong_t)DVA_GET_ASIZE(&bp->blk_dva[d]));
	}
	(void) printf("\n");
}

/*
 * The gang header of bp as it lies on a leaf that holds a whole copy, and
 * its children as targets.
 */
static void
gang_children(spa_t *spa, const char *name, const blkptr_t *bp)
{
	uint64_t hsize = SPA_OLD_GANGBLOCKSIZE;
	const char *bad, *ref = NULL;
	char path[PATH_BUF], cname[24];
	uint8_t *buf;
	uint64_t off = 0;
	int fd, found = 0;

	if (spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER)) {
		hsize = UINT64_MAX;
		for (int d = 0; d < BP_GET_NDVAS(bp); d++) {
			vdev_t *vd = vdev_lookup_top(spa,
			    DVA_GET_VDEV(&bp->blk_dva[d]));
			hsize = MIN(hsize, vdev_gang_header_psize(vd));
		}
	}
	for (int d = 0; d < BP_GET_NDVAS(bp) && ref == NULL; d++) {
		if (dva_leaves(spa, &bp->blk_dva[d], &bad, &ref))
			off = DVA_GET_OFFSET(&bp->blk_dva[d]);
	}
	VERIFY3P(ref, !=, NULL);
	buf = malloc(hsize);
	leaf_path(path, sizeof (path), ref);
	fd = open(path, O_RDONLY);
	VERIFY3S(fd, !=, -1);
	VERIFY3S(pread(fd, buf, hsize, off + VDEV_LABEL_START_SIZE), ==,
	    hsize);
	(void) close(fd);
	VERIFY3U(gbh_eck(buf, hsize)->zec_magic, ==, ZEC_MAGIC);
	(void) printf("INFO %s: gang header of %llu bytes read from %s at "
	    "%llx\n", name, (u_longlong_t)hsize, ref, (u_longlong_t)off);
	(void) snprintf(cname, sizeof (cname), "%s/c", name);
	for (int k = 0; k < gbh_nblkptrs(hsize); k++) {
		blkptr_t *cbp = gbh_bp(buf, k);

		if (BP_IS_HOLE(cbp))
			continue;
		VERIFY(!BP_IS_GANG(cbp));
		print_bp(cname, cbp);
		add_targets(spa, "gchild", name, cbp);
		found++;
	}
	VERIFY3S(found, >, 0);
	free(buf);
}

static void
write_objects(spa_t *spa, objset_t *os, const char *prefix, int n,
    uint64_t seed0, uint64_t copies, boolean_t gang)
{
	VERIFY0(dsl_prop_set_int(POOL, "copies", ZPROP_SRC_LOCAL, copies));
	if (gang) {
		metaslab_force_ganging = 100 << 10;
		metaslab_force_ganging_pct = 100;
	}
	for (int i = 0; i < n; i++) {
		char name[16];
		uint64_t obj;
		blkptr_t bp;

		(void) snprintf(name, sizeof (name), "%s%02d", prefix, i);
		obj = put_object(os, seed0 + i);
		txg_wait_synced(spa_get_dsl(spa), 0);
		bp = object_bp(os, obj);
		(void) printf("INFO %s is object %llu\n", name,
		    (u_longlong_t)obj);
		print_bp(name, &bp);
		VERIFY3S(BP_IS_GANG(&bp), ==, gang);
		spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
		add_targets(spa, gang ? "ghdr" : "plain", name, &bp);
		if (gang)
			gang_children(spa, name, &bp);
		spa_config_exit(spa, SCL_VDEV, FTAG);
	}
	metaslab_force_ganging = SPA_MAXBLOCKSIZE + 1;
	metaslab_force_ganging_pct = 3;
}

static void
save_targets(void)
{
	char path[PATH_BUF];
	FILE *fp;

	leaf_path(path, sizeof (path), "targets");
	fp = fopen(path, "w");
	VERIFY3P(fp, !=, NULL);
	for (int i = 0; i < ntg; i++) {
		target_t *t = &tg[i];

		(void) fprintf(fp, "%s %s %d %d %llu %llx %s %s\n", t->t_kind,
		    t->t_name, t->t_dva, t->t_ndvas,
		    (u_longlong_t)t->t_vdev, (u_longlong_t)t->t_off,
		    t->t_bad, t->t_ref);
	}
	VERIFY0(fclose(fp));
}

static void
load_targets(void)
{
	char path[PATH_BUF];
	unsigned long long vdev, off;
	FILE *fp;

	leaf_path(path, sizeof (path), "targets");
	fp = fopen(path, "r");
	VERIFY3P(fp, !=, NULL);
	ntg = 0;
	while (ntg < MAXT) {
		target_t *t = &tg[ntg];

		if (fscanf(fp, "%7s %15s %d %d %llu %llx %7s %7s", t->t_kind,
		    t->t_name, &t->t_dva, &t->t_ndvas, &vdev, &off, t->t_bad,
		    t->t_ref) != 8)
			break;
		t->t_vdev = vdev;
		t->t_off = off;
		ntg++;
	}
	(void) fclose(fp);
}

static int
do_setup(boolean_t single, boolean_t c2gang)
{
	objset_t *os;
	spa_t *spa;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	create_pool(single);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	if (!single)
		add_mirror(spa);
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG,
	    &os));
	write_objects(spa, os, "plain-", NPLAIN, 100, 2, B_FALSE);
	write_objects(spa, os, "gang-", NGANG, 200, 1, B_TRUE);
	if (c2gang)
		write_objects(spa, os, "gang2-", NGANG, 300, 2, B_TRUE);
	dmu_objset_disown(os, B_TRUE, FTAG);
	VERIFY0(dsl_prop_set_int(POOL, "copies", ZPROP_SRC_LOCAL, 1));
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("INFO dynamic_gang_header %s\n",
	    spa_feature_is_active(spa, SPA_FEATURE_DYNAMIC_GANG_HEADER) ?
	    "active" : "not active");
	export_pool(spa);
	kernel_fini();
	save_targets();
	(void) printf("INFO %d targets\n", ntg);
	return (0);
}

static int
do_damage(boolean_t first)
{
	uint64_t size = 1ULL << ashift;
	uint8_t *buf = malloc(size);
	char path[PATH_BUF];
	int ofd, n = 0;

	load_targets();
	leaf_path(path, sizeof (path), "orig");
	ofd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(ofd, !=, -1);
	for (int i = 0; i < ntg; i++) {
		target_t *t = &tg[i];
		uint64_t off = t->t_off + VDEV_LABEL_START_SIZE;
		int fd;

		if ((t->t_dva == 0) != first || t->t_ndvas < 2)
			continue;
		leaf_path(path, sizeof (path), t->t_bad);
		fd = open(path, O_RDWR);
		VERIFY3S(fd, !=, -1);
		VERIFY3S(pread(fd, buf, size, off), ==, size);
		VERIFY3S(write(ofd, buf, size), ==, size);
		for (uint64_t k = 0; k < size; k++)
			buf[k] = ~buf[k];
		VERIFY3S(pwrite(fd, buf, size, off), ==, size);
		VERIFY0(fsync(fd));
		(void) close(fd);
		(void) printf("INFO damaged %s %s DVA %d of %d on %s at %llx\n",
		    t->t_kind, t->t_name, t->t_dva, t->t_ndvas, t->t_bad,
		    (u_longlong_t)t->t_off);
		n++;
	}
	VERIFY0(close(ofd));
	leaf_path(path, sizeof (path), "damaged");
	ofd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(ofd, !=, -1);
	VERIFY3S(write(ofd, first ? "first\n" : "others\n",
	    first ? 6 : 7), >, 0);
	VERIFY0(close(ofd));
	free(buf);
	(void) printf("INFO damaged %d sectors\n", n);
	return (0);
}

static void
leaf_cksums(vdev_t *vd)
{
	if (vd->vdev_ops->vdev_op_leaf) {
		(void) printf("MEASURE leaf %s: checksum errors %llu, read "
		    "errors %llu, read bytes %llu\n",
		    strrchr(vd->vdev_path, '/') + 1,
		    (u_longlong_t)vd->vdev_stat.vs_checksum_errors,
		    (u_longlong_t)vd->vdev_stat.vs_read_errors,
		    (u_longlong_t)vd->vdev_stat.vs_bytes[ZIO_TYPE_READ]);
	}
	for (int c = 0; c < vd->vdev_children; c++)
		leaf_cksums(vd->vdev_child[c]);
}

static int
do_scrub(boolean_t legacy)
{
	pool_scan_stat_t ps;
	int sorted = -1, n;
	spa_t *spa;
	dsl_pool_t *dp;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (legacy)
		VERIFY0(handle_tunable_option("zfs_scan_legacy=1", B_FALSE));
	spa = import_pool();
	dp = spa_get_dsl(spa);
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	for (n = 0; n < SCRUB_TXGS && dsl_scan_scrubbing(dp); n++) {
		if (sorted == -1 && dp->dp_scan->scn_phys.scn_state ==
		    DSS_SCANNING)
			sorted = dp->dp_scan->scn_is_sorted;
		txg_wait_synced(dp, 0);
	}
	VERIFY0(spa_scan_get_stats(spa, &ps));
	(void) printf("MEASURE scrub: legacy=%d sorted=%d state=%llu "
	    "examined=%llu issued=%llu errors=%llu txgs=%d\n", legacy, sorted,
	    (u_longlong_t)ps.pss_state, (u_longlong_t)ps.pss_examined,
	    (u_longlong_t)ps.pss_issued, (u_longlong_t)ps.pss_errors, n);
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	leaf_cksums(spa->spa_root_vdev);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	if (getenv("GSP_DBGMSG") != NULL) {
		(void) fflush(stdout);
		zfs_dbgmsg_print(STDOUT_FILENO, "gang_scrub_probe");
	}
	export_pool(spa);
	kernel_fini();
	return (ps.pss_state == DSS_FINISHED && ps.pss_errors == 0 ? 0 : 1);
}

static int
do_check(void)
{
	uint64_t size = 1ULL << ashift;
	uint8_t *buf = malloc(size), *orig = malloc(size);
	const char *kinds[] = { "plain", "ghdr", "gchild" };
	int fixed[3] = { 0 }, total[3] = { 0 };
	char path[PATH_BUF], mode[16] = "";
	boolean_t first;
	FILE *fp;
	int ofd, left = 0;

	load_targets();
	leaf_path(path, sizeof (path), "damaged");
	fp = fopen(path, "r");
	VERIFY3P(fp, !=, NULL);
	VERIFY3S(fscanf(fp, "%15s", mode), ==, 1);
	(void) fclose(fp);
	first = strcmp(mode, "first") == 0;
	leaf_path(path, sizeof (path), "orig");
	ofd = open(path, O_RDONLY);
	VERIFY3S(ofd, !=, -1);
	for (int i = 0; i < ntg; i++) {
		target_t *t = &tg[i];
		uint64_t off = t->t_off + VDEV_LABEL_START_SIZE;
		boolean_t ok, refok = B_TRUE;
		int fd, k;

		if ((t->t_dva == 0) != first || t->t_ndvas < 2)
			continue;
		VERIFY3S(read(ofd, orig, size), ==, size);
		leaf_path(path, sizeof (path), t->t_bad);
		fd = open(path, O_RDONLY);
		VERIFY3S(fd, !=, -1);
		VERIFY3S(pread(fd, buf, size, off), ==, size);
		(void) close(fd);
		ok = memcmp(buf, orig, size) == 0;
		if (strcmp(t->t_ref, t->t_bad) != 0) {
			leaf_path(path, sizeof (path), t->t_ref);
			fd = open(path, O_RDONLY);
			VERIFY3S(fd, !=, -1);
			VERIFY3S(pread(fd, buf, size, off), ==, size);
			(void) close(fd);
			refok = memcmp(buf, orig, size) == 0;
		}
		k = strcmp(t->t_kind, "plain") == 0 ? 0 :
		    strcmp(t->t_kind, "ghdr") == 0 ? 1 : 2;
		total[k]++;
		fixed[k] += ok;
		(void) printf("INFO %s %s DVA %d of %d on %s at %llx: %s%s\n",
		    t->t_kind, t->t_name, t->t_dva, t->t_ndvas, t->t_bad,
		    (u_longlong_t)t->t_off, ok ? "repaired" : "still damaged",
		    refok ? "" : " (and the reference leaf differs!)");
	}
	(void) close(ofd);
	for (int k = 0; k < 3; k++) {
		(void) printf("RESULT damaged DVA %s: %s %d of %d repaired\n",
		    first ? "0" : ">0", kinds[k], fixed[k], total[k]);
		left += total[k] - fixed[k];
	}
	free(buf);
	free(orig);
	return (left == 0 ? 0 : 1);
}

static int
usage(void)
{
	(void) fprintf(stderr, "usage: gang_scrub_probe <dir> <ashift> "
	    "setup <rzmirror|single> [c2gang]\n"
	    "       gang_scrub_probe <dir> <ashift> damage <others|first>\n"
	    "       gang_scrub_probe <dir> <ashift> scrub <sorted|legacy>\n"
	    "       gang_scrub_probe <dir> <ashift> check\n");
	return (2);
}

int
main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 4)
		return (usage());
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 10);
	cmd = argv[3];
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	if (strcmp(cmd, "setup") == 0 && argc >= 5) {
		return (do_setup(strcmp(argv[4], "single") == 0,
		    argc > 5 && strcmp(argv[5], "c2gang") == 0));
	} else if (strcmp(cmd, "damage") == 0 && argc == 5) {
		return (do_damage(strcmp(argv[4], "first") == 0));
	} else if (strcmp(cmd, "scrub") == 0 && argc == 5) {
		return (do_scrub(strcmp(argv[4], "legacy") == 0));
	} else if (strcmp(cmd, "check") == 0) {
		return (do_check());
	}
	return (usage());
}
