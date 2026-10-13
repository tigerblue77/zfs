// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 7 (throwaway, not part of any tree): what does the redirect
 * table of a raidz vdev cost at scale?  The plan's targets, for 1.7e7
 * entries on one raidz vdev: a lookup in 1 microsecond (p99), at most 1 GiB
 * of memory, an import (table load) in 5 seconds; and what the table costs
 * every raidz I/O it does not redirect.
 *
 *   scale_probe <dir> <ashift> <command> [args] [<command> [args] ...]
 *
 * The commands run in order in one process, against the pool "scl" of five
 * sparse files in <dir> (SCALE_FILE_GB each, 4 by default):
 *
 *   create <MiB>        a raidz1 pool, <MiB> of incompressible data in four
 *                       objects of 16K records written at parity 1 with
 *                       primarycache=none, then a parity-2 epoch; the data
 *                       region and the parameters go to <dir>/params.
 *   import <ro|rw> [cold]  find the pool (a tryimport) and import it, timed;
 *                       cold first drops the page cache.
 *   synth <from> <n> <disk|hot|mem> <batch> [sorted]  add synthetic
 *                       entries from..from+n-1 (see gen_entry(); from must
 *                       be the number already there) with
 *                       vdev_raidz_redirect_synth(), in sync tasks of
 *                       <batch> entries, in key order with sorted; disk
 *                       persists them with their stored block pointers,
 *                       hot without them where the format can, mem in
 *                       memory only.
 *   lookup <threads>    time 1e6 lookups per thread with the I/O path's own
 *                       vdev_raidz_redirect_get(): hits (random entries),
 *                       misses (random even sectors, old births) and new
 *                       (random offsets, births after the epoch), by one
 *                       thread, then by <threads> at once.
 *   memory [reload]     what the table holds in malloc's in-use bytes
 *                       (mallinfo2, all arenas): before and after
 *                       vdev_raidz_redirect_unload(); reload reads it again.
 *   sync <n> <rounds>   add <n> new persisted entries in one sync task and
 *                       remove them in another, <rounds> times, and print
 *                       what spa_sync() took for those txgs and an empty one.
 *   read <passes> <threads>  read the data objects (dmu_read, no prefetch,
 *                       ARC bypassed by primarycache=none) <passes> times
 *                       with <threads> threads; wall time, CPU time and the
 *                       raidz vdev's read count.
 *   ondisk              what the table's objects hold and take on disk.
 *   condense            condense the log now (the redesign's format).
 *   stress <seconds> <readers> <churn>  while <readers> threads look up
 *                       entries [0, n) and <churn> other keys in the same
 *                       metaslabs, add those keys in one sync task, remove
 *                       them in another and let their tombstones expire,
 *                       again and again for <seconds>: the shards grow,
 *                       shrink and move slots under the lock-free readers.
 *                       Every lookup of an entry must find it, with its
 *                       fields, and one of the other keys, when it finds
 *                       it, must return that key's own fields.
 *                       SCALE_UNSAFE sets spike_redirect_unsafe_readers.
 *   export
 *
 * Environment: SCALE_UNIVERSE sets U (see gen_entry()); SCALE_CONDENSE sets
 * spike_redirect_condense (-1: never condense the log);
 * SCALE_ARC_MB caps the ARC (1024 by default, with zfs_arc_min lowered so
 * that userland's arc_init() does not ignore the cap); SCALE_LOAD_SKIP
 * sets spike_redirect_load_skip, so that the table loads empty.  Spike
 * symbols are weak: read works on a libzpool without the spike, to measure
 * the base commit.  Lines starting with MEASURE carry the numbers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/zfs_context.h>
#include <sys/arc_impl.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dnode.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_synctask.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/txg.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <linux/falloc.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzutil.h>
#include <libzpool.h>

#define	POOL		"scl"
#define	WIDTH		5
#define	RECSZ		(16ULL << 10)
#define	NOBJ		4
#define	NQ		1000000
#define	OLD_SECTORS	320	/* a 128K block at parity 1, 5 wide */
#define	NEW_SECTORS	384
#define	PATH_BUF	512

extern uint64_t zfs_arc_max;
extern uint64_t zfs_arc_min;

/* Weak: absent from the base commit's libzpool. */
extern int vdev_raidz_redirect_synth(spa_t *, uint64_t,
    raidz_redirect_ent_t *, uint64_t, int, uint64_t *) __attribute__((weak));
extern void vdev_raidz_redirect_unload(spa_t *, uint64_t)
    __attribute__((weak));
extern int vdev_raidz_redirect_reload(spa_t *, uint64_t)
    __attribute__((weak));
extern boolean_t vdev_raidz_redirect_get(vdev_t *, uint64_t, uint64_t,
    raidz_redirect_ent_t *) __attribute__((weak));
extern uint64_t vdev_raidz_redirect_count(spa_t *, uint64_t)
    __attribute__((weak));
extern uint64_t vdev_raidz_redirect_hits(void) __attribute__((weak));
extern int spike_redirect_load_skip __attribute__((weak));
extern spike_table_stat_t spike_table_stats __attribute__((weak));
extern int spike_redirect_condense __attribute__((weak));
extern int spike_redirect_unsafe_readers __attribute__((weak));

typedef struct params {
	uint64_t	p_ashift;
	uint64_t	p_asize;	/* the raidz vdev's */
	uint64_t	p_ms_shift;
	uint64_t	p_usable;	/* end of its last whole metaslab */
	uint64_t	p_data_start;	/* the data blocks' extents */
	uint64_t	p_data_end;
	uint64_t	p_epoch;	/* first txg of parity 2 */
	uint64_t	p_data_birth_max;
	uint64_t	p_n;		/* synthetic entries added */
	uint64_t	p_obj[NOBJ];
	uint64_t	p_obj_size;
} params_t;

static const char *dir;
static uint64_t ashift;
static spa_t *spa;
static params_t P;
static uint64_t file_size = 4ULL << 30;
/* Entries [0, live_n) are in this process's table. */
static uint64_t live_n;

static uint64_t
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

static uint64_t
cpu_ns(void)
{
	struct rusage ru;

	(void) getrusage(RUSAGE_SELF, &ru);
	return ((ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ULL +
	    (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ULL);
}

static size_t
heap_used(void)
{
	struct mallinfo2 mi = mallinfo2();

	return (mi.uordblks + mi.hblkhd);
}

static uint64_t
rss_bytes(void)
{
	unsigned long size = 0, rss = 0;
	FILE *f = fopen("/proc/self/statm", "r");

	if (f != NULL) {
		if (fscanf(f, "%lu %lu", &size, &rss) != 2)
			rss = 0;
		(void) fclose(f);
	}
	return ((uint64_t)rss * sysconf(_SC_PAGESIZE));
}

/* The "Cached:" line of /proc/meminfo: the host's page cache. */
static uint64_t
page_cache_bytes(void)
{
	char line[256];
	unsigned long long kb = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	while (f != NULL && fgets(line, sizeof (line), f) != NULL) {
		if (sscanf(line, "Cached: %llu kB", &kb) == 1)
			break;
	}
	if (f != NULL)
		(void) fclose(f);
	return ((uint64_t)kb << 10);
}

static uint64_t
mix(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return (x ^ (x >> 31));
}

static void
params_save(void)
{
	char path[PATH_BUF];
	FILE *f;

	(void) snprintf(path, sizeof (path), "%s/params", dir);
	VERIFY3P((f = fopen(path, "w")), !=, NULL);
	(void) fprintf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
	    (u_longlong_t)P.p_ashift, (u_longlong_t)P.p_asize,
	    (u_longlong_t)P.p_ms_shift, (u_longlong_t)P.p_usable,
	    (u_longlong_t)P.p_data_start, (u_longlong_t)P.p_data_end,
	    (u_longlong_t)P.p_epoch, (u_longlong_t)P.p_data_birth_max,
	    (u_longlong_t)P.p_n, (u_longlong_t)P.p_obj_size);
	for (int i = 0; i < NOBJ; i++)
		(void) fprintf(f, "%llu\n", (u_longlong_t)P.p_obj[i]);
	(void) fclose(f);
}

static void
params_load(void)
{
	char path[PATH_BUF];
	unsigned long long v[10];
	FILE *f;

	(void) snprintf(path, sizeof (path), "%s/params", dir);
	VERIFY3P((f = fopen(path, "r")), !=, NULL);
	VERIFY3S(fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
	    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8],
	    &v[9]), ==, 10);
	P.p_ashift = v[0];
	P.p_asize = v[1];
	P.p_ms_shift = v[2];
	P.p_usable = v[3];
	P.p_data_start = v[4];
	P.p_data_end = v[5];
	P.p_epoch = v[6];
	P.p_data_birth_max = v[7];
	P.p_n = v[8];
	P.p_obj_size = v[9];
	for (int i = 0; i < NOBJ; i++) {
		unsigned long long o;

		VERIFY3S(fscanf(f, "%llu", &o), ==, 1);
		P.p_obj[i] = o;
	}
	(void) fclose(f);
}

/*
 * Synthetic entry j of a universe of U (SCALE_UNIVERSE, 1.7e7 by default),
 * so that the first n entries of any run are the same and spread evenly
 * over the vdev: slot (j * PERM_A) mod U, each slot STRIDE pairs of sectors
 * wide.  Its old offset is the odd sector starting the slot, where no
 * extent of the parity-1 data starts (raidz1 rounds every allocation to two
 * sectors), so that no real block has its key.  Its birth is old (before
 * the parity-2 epoch), its old extent and N lie in one metaslab each, half a
 * vdev apart.
 */
#define	PERM_A		2654435761ULL

static uint64_t universe = 17000000ULL;

static uint64_t
slot_pairs(void)
{
	return ((P.p_usable >> (P.p_ashift + 1)) / universe);
}

static void
gen_entry(uint64_t j, raidz_redirect_ent_t *e)
{
	uint64_t sec = 1ULL << P.p_ashift;
	uint64_t k = (j * PERM_A) % universe;
	uint64_t o, no, ms, left;

	o = (2 * k * slot_pairs() + 1) << P.p_ashift;
	ms = 1ULL << P.p_ms_shift;
	left = (P2ROUNDUP(o + 1, ms) - o) >> P.p_ashift;
	e->rde_offset = o;
	e->rde_birth = 2 + mix(j) % (P.p_epoch - 2);
	e->rde_old_asize = MIN(OLD_SECTORS, left) << P.p_ashift;
	e->rde_old_reserved = sec;
	e->rde_new_asize = NEW_SECTORS << P.p_ashift;
	no = (o + P.p_usable / 2) % P.p_usable;
	no &= ~((2ULL << P.p_ashift) - 1);
	if (P2ROUNDUP(no + 1, ms) - no < e->rde_new_asize)
		no = P2ROUNDUP(no + 1, ms) - e->rde_new_asize;
	e->rde_new_offset = no;
	e->rde_new_birth = 0;	/* the task's txg */
}

/*
 * A key no synthetic entry has: the odd sector after entry j's (slots are
 * at least two pairs wide), with a one-sector old extent.
 */
static void
gen_other(uint64_t j, raidz_redirect_ent_t *e)
{
	gen_entry(j, e);
	e->rde_offset += 2ULL << P.p_ashift;
	e->rde_old_asize = 1ULL << P.p_ashift;
	if ((e->rde_offset >> P.p_ms_shift) !=
	    ((e->rde_offset - (2ULL << P.p_ashift)) >> P.p_ms_shift))
		e->rde_offset = 0;
}

static void
leaf_path(char *buf, size_t len, int c)
{
	(void) snprintf(buf, len, "%s/v%d", dir, c);
}

static void
fill_random(uint64_t *buf, size_t size, uint64_t seed)
{
	uint64_t x = mix(seed);

	for (size_t i = 0; i < size / 8; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buf[i] = x;
	}
}

static void punch_free(void);

typedef struct epoch_arg {
	uint64_t	ea_table[6];
} epoch_arg_t;

static int
epoch_check(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	if (!dmu_tx_is_syncing(tx))
		return (0);
	ea->ea_table[3] = vdev_raidz_parity_epochs_first_start(tx);
	return (vdev_raidz_parity_epochs_set_check_tx(vd, ea->ea_table, 2, tx,
	    NULL));
}

static void
epoch_sync(void *arg, dmu_tx_t *tx)
{
	epoch_arg_t *ea = arg;
	vdev_t *vd = dmu_tx_pool(tx)->dp_spa->spa_root_vdev->vdev_child[0];

	VERIFY0(vdev_raidz_parity_epochs_set(vd, ea->ea_table, 2, tx));
}

static void
cmd_create(uint64_t mib)
{
	nvlist_t *child[WIDTH], *raid, *root, *props;
	char path[PATH_BUF];
	objset_t *os;
	uint64_t *buf = malloc(1 << 20);
	epoch_arg_t ea = { { 0, WIDTH, 1, 0, WIDTH, 2 } };
	vdev_t *vd;
	uint64_t t0 = now_ns();

	for (int c = 0; c < WIDTH; c++) {
		int fd;

		leaf_path(path, sizeof (path), c);
		fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		VERIFY3S(fd, !=, -1);
		VERIFY0(ftruncate(fd, file_size));
		(void) close(fd);
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
		fnvlist_add_string(child[c], ZPOOL_CONFIG_PATH, path);
		fnvlist_add_uint64(child[c], ZPOOL_CONFIG_ASHIFT, ashift);
	}
	raid = fnvlist_alloc();
	fnvlist_add_string(raid, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_uint64(raid, ZPOOL_CONFIG_IS_LOG, 0);
	fnvlist_add_nvlist_array(raid, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, WIDTH);
	root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)&raid, 1);
	props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char name[256];

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		(void) snprintf(name, sizeof (name), "feature@%s",
		    spa_feature_table[i].fi_uname);
		fnvlist_add_uint64(props, name, 0);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raid);
	for (int c = 0; c < WIDTH; c++)
		fnvlist_free(child[c]);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	VERIFY0(dsl_prop_set_int(POOL, "compression", ZPROP_SRC_LOCAL,
	    ZIO_COMPRESS_OFF));
	VERIFY0(dsl_prop_set_int(POOL, "primarycache", ZPROP_SRC_LOCAL,
	    ZFS_CACHE_NONE));

	memset(&P, 0, sizeof (P));
	P.p_ashift = ashift;
	P.p_obj_size = (mib << 20) / NOBJ;
	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_FALSE, B_TRUE, FTAG, &os));
	for (int i = 0; i < NOBJ; i++) {
		dmu_tx_t *tx = dmu_tx_create(os);

		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
		VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
		P.p_obj[i] = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS,
		    RECSZ, DMU_OT_NONE, 0, tx);
		dmu_tx_commit(tx);
		for (uint64_t off = 0; off < P.p_obj_size; off += 1 << 20) {
			tx = dmu_tx_create(os);
			dmu_tx_hold_write(tx, P.p_obj[i], off, 1 << 20);
			VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
			fill_random(buf, 1 << 20,
			    ((uint64_t)i << 32) | (off >> 20));
			dmu_write(os, P.p_obj[i], off, 1 << 20, buf, tx,
			    DMU_READ_NO_PREFETCH);
			dmu_tx_commit(tx);
		}
	}
	txg_wait_synced(spa_get_dsl(spa), 0);

	/* Where the data blocks lie, and their births. */
	P.p_data_start = UINT64_MAX;
	for (int i = 0; i < NOBJ; i++) {
		for (uint64_t off = 0; off < P.p_obj_size; off += RECSZ) {
			dmu_buf_t *db;
			const blkptr_t *bp;
			uint64_t o, a;

			VERIFY0(dmu_buf_hold(os, P.p_obj[i], off, FTAG, &db,
			    DMU_READ_NO_PREFETCH));
			bp = dmu_buf_get_blkptr(db);
			o = DVA_GET_OFFSET(&bp->blk_dva[0]);
			a = DVA_GET_ASIZE(&bp->blk_dva[0]);
			P.p_data_start = MIN(P.p_data_start, o);
			P.p_data_end = MAX(P.p_data_end, o + a);
			P.p_data_birth_max = MAX(P.p_data_birth_max,
			    BP_GET_PHYSICAL_BIRTH(bp));
			dmu_buf_rele(db, FTAG);
		}
	}
	dmu_objset_disown(os, B_TRUE, FTAG);

	VERIFY0(dsl_sync_task(POOL, epoch_check, epoch_sync, &ea, 0,
	    ZFS_SPACE_CHECK_NONE));
	txg_wait_synced(spa_get_dsl(spa), ea.ea_table[3]);
	P.p_epoch = ea.ea_table[3];
	vd = spa->spa_root_vdev->vdev_child[0];
	P.p_asize = vd->vdev_asize;
	P.p_ms_shift = vd->vdev_ms_shift;
	P.p_usable = (vd->vdev_asize >> vd->vdev_ms_shift) <<
	    vd->vdev_ms_shift;
	params_save();
	punch_free();
	(void) printf("INFO created: %llu MiB in %d objects of %llu-byte "
	    "records, data in [%llx, %llx) born up to txg %llu, parity 2 "
	    "from txg %llu, vdev asize %llu, ms_shift %llu, %llu metaslabs, "
	    "%.1f s\n", (u_longlong_t)mib, NOBJ, (u_longlong_t)RECSZ,
	    (u_longlong_t)P.p_data_start, (u_longlong_t)P.p_data_end,
	    (u_longlong_t)P.p_data_birth_max, (u_longlong_t)P.p_epoch,
	    (u_longlong_t)P.p_asize, (u_longlong_t)P.p_ms_shift,
	    (u_longlong_t)vd->vdev_ms_count, (now_ns() - t0) / 1e9);
	free(buf);
}

static void
print_load_stats(const char *what)
{
	if (&spike_table_stats == NULL)
		return;
	(void) printf("MEASURE %s table loads=%llu last=%.3f s total=%.3f s "
	    "entries=%llu records=%llu\n", what,
	    (u_longlong_t)spike_table_stats.sts_load_calls,
	    spike_table_stats.sts_load_ns / 1e9,
	    spike_table_stats.sts_load_total_ns / 1e9,
	    (u_longlong_t)spike_table_stats.sts_load_entries,
	    (u_longlong_t)spike_table_stats.sts_load_records);
#ifdef	RAIDZ_REDIRECT_REC_ADD
	/* Only the redesign's libzpool has these. */
	if (&spike_redirect_condense != NULL) {
		(void) printf("MEASURE %s table load in dmu_read=%.3f s\n",
		    what, spike_table_stats.sts_load_read_ns / 1e9);
	}
#endif
}

static void
cmd_import(const char *how, boolean_t cold)
{
	importargs_t ia;
	char *paths[1];
	nvlist_t *config = NULL;
	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};
	uint64_t t0, t1, t2, l0 = 0, l1 = 0, rss0;
	nvlist_t *tryconf;
	int err;

	if (cold) {
		uint64_t c0 = page_cache_bytes();
		int fd;

		sync();
		fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
		VERIFY3S(fd, >=, 0);
		VERIFY3S(write(fd, "3", 1), ==, 1);
		(void) close(fd);
		(void) printf("INFO page cache %llu bytes, %llu after "
		    "drop_caches\n", (u_longlong_t)c0,
		    (u_longlong_t)page_cache_bytes());
	}
	rss0 = rss_bytes();
	memset(&ia, 0, sizeof (ia));
	paths[0] = (char *)dir;
	ia.path = paths;
	ia.paths = 1;
	ia.can_be_active = B_TRUE;
	if (&spike_table_stats != NULL)
		l0 = spike_table_stats.sts_load_total_ns;
	t0 = now_ns();
	VERIFY0(zpool_find_config(&lpch, POOL, &config, &ia));
	/* What "zpool import" asks the kernel for first. */
	tryconf = spa_tryimport(config);
	VERIFY3P(tryconf, !=, NULL);
	fnvlist_free(tryconf);
	t1 = now_ns();
	if (&spike_table_stats != NULL)
		l1 = spike_table_stats.sts_load_total_ns;
	err = spa_import((char *)POOL, config, NULL, ZFS_IMPORT_NORMAL);
	t2 = now_ns();
	fnvlist_free(config);
	VERIFY0(err);
	VERIFY0(spa_open(POOL, &spa, FTAG));
	params_load();
	/* With the load skipped, the table starts empty. */
	live_n = getenv("SCALE_LOAD_SKIP") != NULL ? 0 : P.p_n;
	(void) printf("MEASURE import %s%s find+tryimport=%.3f s import=%.3f s "
	    "total=%.3f s rss_delta=%llu\n", how, cold ? " cold" : " warm",
	    (t1 - t0) / 1e9, (t2 - t1) / 1e9, (t2 - t0) / 1e9,
	    (u_longlong_t)(rss_bytes() - rss0));
	if (&spike_table_stats != NULL) {
		(void) printf("MEASURE import table load in find+tryimport="
		    "%.3f s in import=%.3f s\n", (l1 - l0) / 1e9,
		    (spike_table_stats.sts_load_total_ns - l1) / 1e9);
	}
	print_load_stats("import");
}

static void
cmd_export(void)
{
	uint64_t t0 = now_ns();

	spa_close(spa, FTAG);
	VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
	spa = NULL;
	(void) printf("INFO exported in %.3f s\n", (now_ns() - t0) / 1e9);
}

static uint64_t
sync_ns(uint64_t txg)
{
	uint64_t i = txg % SPIKE_REDIRECT_TXGS;

	if (spike_table_stats.sts_sync_txg[i] != txg)
		return (0);
	return (spike_table_stats.sts_sync_ns[i]);
}

/*
 * The pool's files are sparse and nothing gives back to the host what the
 * pool frees, and a TRIM skips extents under 32K, which is most of what a
 * table rewritten leaf by leaf frees.  So punch every free range of every
 * metaslab ever allocated out of the leaf files, as a TRIM would without
 * that floor: vdev_xlate_walk() maps it to the rows of each child that
 * hold only free sectors.  The pool is idle; the metaslab's lock keeps
 * allocations out while it is done.  A free stays in the defer trees for
 * TXG_DEFER_SIZE txgs: sync a few first.
 */
typedef struct punch_arg {
	int		pa_fd;
	uint64_t	pa_bytes;
	uint64_t	pa_calls;
	vdev_t		*pa_tvd;
} punch_arg_t;

static void
punch_phys(void *arg, zfs_range_seg64_t *prs)
{
	punch_arg_t *pa = arg;

	if (prs->rs_end <= prs->rs_start)
		return;
	VERIFY0(fallocate(pa->pa_fd, FALLOC_FL_PUNCH_HOLE |
	    FALLOC_FL_KEEP_SIZE, prs->rs_start + VDEV_LABEL_START_SIZE,
	    prs->rs_end - prs->rs_start));
	pa->pa_bytes += prs->rs_end - prs->rs_start;
	pa->pa_calls++;
}

static void
punch_seg(void *arg, uint64_t start, uint64_t size)
{
	punch_arg_t *pa = arg;
	zfs_range_seg64_t lrs = { .rs_start = start, .rs_end = start + size };

	for (uint64_t c = 0; c < pa->pa_tvd->vdev_children; c++) {
		vdev_xlate_walk(pa->pa_tvd->vdev_child[c], &lrs, punch_phys,
		    &pa[1 + c]);
	}
}

static void
punch_free(void)
{
	vdev_t *tvd = spa->spa_root_vdev->vdev_child[0];
	punch_arg_t pa[1 + WIDTH];
	uint64_t t0 = now_ns(), bytes = 0, calls = 0, ms = 0;

	for (int i = 0; i < TXG_DEFER_SIZE + 1; i++)
		txg_wait_synced(spa_get_dsl(spa), 0);
	memset(pa, 0, sizeof (pa));
	pa[0].pa_tvd = tvd;
	for (int c = 0; c < WIDTH; c++) {
		char path[PATH_BUF];

		leaf_path(path, sizeof (path), c);
		pa[1 + c].pa_fd = open(path, O_RDWR);
		VERIFY3S(pa[1 + c].pa_fd, >=, 0);
	}
	spa_config_enter(spa, SCL_CONFIG, FTAG, RW_READER);
	for (uint64_t m = 0; m < tvd->vdev_ms_count; m++) {
		metaslab_t *msp = tvd->vdev_ms[m];

		mutex_enter(&msp->ms_lock);
		if (msp->ms_sm != NULL && metaslab_load(msp) == 0) {
			zfs_range_tree_walk(msp->ms_allocatable, punch_seg,
			    pa);
			ms++;
		}
		mutex_exit(&msp->ms_lock);
	}
	spa_config_exit(spa, SCL_CONFIG, FTAG);
	for (int c = 0; c < WIDTH; c++) {
		bytes += pa[1 + c].pa_bytes;
		calls += pa[1 + c].pa_calls;
		(void) close(pa[1 + c].pa_fd);
	}
	(void) printf("INFO punched %llu bytes in %llu ranges of %llu "
	    "metaslabs in %.3f s\n", (u_longlong_t)bytes, (u_longlong_t)calls,
	    (u_longlong_t)ms, (now_ns() - t0) / 1e9);
}

static int
ent_compare(const void *a, const void *b)
{
	const raidz_redirect_ent_t *x = a, *y = b;
	int c = TREE_CMP(x->rde_offset, y->rde_offset);

	return (c != 0 ? c : TREE_CMP(x->rde_birth, y->rde_birth));
}

/*
 * Entries from..from+n-1 in key order, so that a log they are appended to
 * is in key order: what a condensed log holds.
 */
static raidz_redirect_ent_t *
sorted_entries(uint64_t from, uint64_t n)
{
	raidz_redirect_ent_t *all = malloc(n * sizeof (*all));
	uint64_t t0 = now_ns();

	VERIFY3P(all, !=, NULL);
	for (uint64_t i = 0; i < n; i++)
		gen_entry(from + i, &all[i]);
	qsort(all, n, sizeof (*all), ent_compare);
	(void) printf("INFO sorted %llu entries in %.1f s\n", (u_longlong_t)n,
	    (now_ns() - t0) / 1e9);
	return (all);
}

static void
cmd_synth(uint64_t from, uint64_t n, const char *how, uint64_t batch,
    boolean_t sorted)
{
	raidz_redirect_ent_t *all = sorted ? sorted_entries(from, n) : NULL;
	int op = strcmp(how, "mem") == 0 ? SPIKE_SYNTH_ADD_MEM :
	    strcmp(how, "hot") == 0 ? SPIKE_SYNTH_ADD_HOT : SPIKE_SYNTH_ADD;
	raidz_redirect_ent_t *e = malloc(batch * sizeof (*e));
	uint64_t t0 = now_ns(), added = 0, rss0 = rss_bytes();
	size_t h0 = heap_used();

	VERIFY3P(vdev_raidz_redirect_synth, !=, NULL);
	VERIFY3U(slot_pairs(), >=, 2);
	VERIFY3U(from, ==, live_n);
	VERIFY3U(from + n, <=, universe);
	for (uint64_t j = from; j < from + n; j += batch) {
		uint64_t m = MIN(batch, from + n - j), done[2] = { 0, 0 };
		uint64_t t1 = now_ns();

		for (uint64_t i = 0; i < m; i++) {
			if (all != NULL)
				e[i] = all[j - from + i];
			else
				gen_entry(j + i, &e[i]);
		}
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, m, op, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		added += done[0];
		(void) printf("MEASURE synth batch %llu+%llu %s added=%llu "
		    "txg=%llu task=%.3f s txg sync=%.3f s table=%llu\n",
		    (u_longlong_t)j, (u_longlong_t)m, how,
		    (u_longlong_t)done[0], (u_longlong_t)done[1],
		    (now_ns() - t1) / 1e9, sync_ns(done[1]) / 1e9,
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
		if (op != SPIKE_SYNTH_ADD_MEM)
			punch_free();
		if (done[0] != m) {
			(void) printf("FAIL batch at %llu added %llu of %llu\n",
			    (u_longlong_t)j, (u_longlong_t)done[0],
			    (u_longlong_t)m);
			exit(1);
		}
		(void) fflush(stdout);
	}
	live_n = from + n;
	if (op != SPIKE_SYNTH_ADD_MEM) {
		P.p_n = live_n;
		params_save();
	}
	(void) printf("MEASURE synth %s from=%llu n=%llu added=%llu "
	    "time=%.1f s heap_delta=%zu rss_delta=%llu table=%llu\n", how,
	    (u_longlong_t)from, (u_longlong_t)n, (u_longlong_t)added,
	    (now_ns() - t0) / 1e9, heap_used() - h0,
	    (u_longlong_t)(rss_bytes() - rss0),
	    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	free(e);
	free(all);
}

typedef struct lq {
	uint64_t	q_offset;
	uint64_t	q_birth;
} lq_t;

typedef struct lrun {
	const lq_t	*r_q;
	uint64_t	*r_lat;
	boolean_t	r_hit;
	uint64_t	r_bad;
	uint64_t	r_ns;
} lrun_t;

static void *
lookup_thread(void *arg)
{
	lrun_t *r = arg;
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	uint64_t t0 = now_ns();

	for (uint64_t i = 0; i < NQ; i++) {
		raidz_redirect_ent_t e;
		uint64_t a = now_ns();
		boolean_t f = vdev_raidz_redirect_get(vd, r->r_q[i].q_offset,
		    r->r_q[i].q_birth, &e);

		r->r_lat[i] = now_ns() - a;
		if (f != r->r_hit)
			r->r_bad++;
	}
	r->r_ns = now_ns() - t0;
	return (NULL);
}

static int
u64cmp(const void *a, const void *b)
{
	return (TREE_CMP(*(const uint64_t *)a, *(const uint64_t *)b));
}

static void
lookup_run(const char *what, const lq_t *q, boolean_t hit, int threads)
{
	lrun_t r[16];
	pthread_t tid[16];
	uint64_t n = (uint64_t)NQ * threads;
	uint64_t *lat = malloc(n * sizeof (uint64_t));
	uint64_t ns = 0, bad = 0, sum = 0;

	for (int t = 0; t < threads; t++) {
		r[t].r_q = q + (uint64_t)t * NQ;
		r[t].r_lat = lat + (uint64_t)t * NQ;
		r[t].r_hit = hit;
		r[t].r_bad = 0;
		r[t].r_ns = 0;
		VERIFY0(pthread_create(&tid[t], NULL, lookup_thread, &r[t]));
	}
	for (int t = 0; t < threads; t++) {
		VERIFY0(pthread_join(tid[t], NULL));
		ns = MAX(ns, r[t].r_ns);
		bad += r[t].r_bad;
	}
	for (uint64_t i = 0; i < n; i++)
		sum += lat[i];
	qsort(lat, n, sizeof (uint64_t), u64cmp);
	(void) printf("MEASURE lookup %s threads=%d n=%llu p50=%llu p90=%llu "
	    "p99=%llu p999=%llu max=%llu ns mean=%.1f ns wrong=%llu "
	    "wall=%.3f s\n", what, threads, (u_longlong_t)n,
	    (u_longlong_t)lat[n / 2], (u_longlong_t)lat[n * 9 / 10],
	    (u_longlong_t)lat[n * 99 / 100], (u_longlong_t)lat[n * 999 / 1000],
	    (u_longlong_t)lat[n - 1], (double)sum / n, (u_longlong_t)bad,
	    ns / 1e9);
	(void) fflush(stdout);
	free(lat);
}

static void
cmd_lookup(int threads)
{
	uint64_t n = live_n, nq = (uint64_t)NQ * threads, timer = 0;
	lq_t *hit = malloc(nq * sizeof (lq_t));
	lq_t *miss = malloc(nq * sizeof (lq_t));
	lq_t *new = malloc(nq * sizeof (lq_t));
	uint64_t s = 4242;

	VERIFY3U(n, >, 0);
	(void) printf("INFO lookup over %llu entries, %llu in the table\n",
	    (u_longlong_t)n, (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
	for (uint64_t i = 0; i < nq; i++) {
		raidz_redirect_ent_t e;

		gen_entry(mix(s++) % n, &e);
		hit[i].q_offset = e.rde_offset;
		hit[i].q_birth = e.rde_birth;
		miss[i].q_offset = ((mix(s++) % (P.p_usable >> P.p_ashift)) &
		    ~1ULL) << P.p_ashift;
		miss[i].q_birth = 2 + mix(s++) % (P.p_epoch - 2);
		new[i].q_offset = (mix(s++) % (P.p_usable >> P.p_ashift)) <<
		    P.p_ashift;
		new[i].q_birth = P.p_epoch + mix(s++) % 1000;
	}
	for (int i = 0; i < 1000000; i++) {
		uint64_t a = now_ns();

		timer += now_ns() - a;
	}
	(void) printf("INFO timer overhead mean=%.1f ns (not subtracted)\n",
	    timer / 1e6);
	lookup_run("hit", hit, B_TRUE, 1);
	lookup_run("miss", miss, B_FALSE, 1);
	lookup_run("new", new, B_FALSE, 1);
	lookup_run("hit", hit, B_TRUE, threads);
	lookup_run("miss", miss, B_FALSE, threads);
	lookup_run("new", new, B_FALSE, threads);
	free(hit);
	free(miss);
	free(new);
}

static void
cmd_memory(boolean_t reload)
{
	uint64_t count = vdev_raidz_redirect_count(spa, 0);
	size_t h0, h1, h2;
	uint64_t r0, r1, r2, t0;

	h0 = heap_used();
	r0 = rss_bytes();
	vdev_raidz_redirect_unload(spa, 0);
	h1 = heap_used();
	r1 = rss_bytes();
	(void) printf("MEASURE memory unload entries=%llu heap_freed=%zu "
	    "(%.1f bytes/entry) rss_freed=%lld\n", (u_longlong_t)count,
	    h0 - h1, count ? (double)(h0 - h1) / count : 0.0,
	    (long long)(r0 - r1));
	if (!reload)
		return;
	t0 = now_ns();
	VERIFY0(vdev_raidz_redirect_reload(spa, 0));
	h2 = heap_used();
	r2 = rss_bytes();
	(void) printf("MEASURE memory reload entries=%llu time=%.3f s "
	    "heap_added=%zu (%.1f bytes/entry, ARC growth included) "
	    "rss_added=%lld\n",
	    (u_longlong_t)vdev_raidz_redirect_count(spa, 0),
	    (now_ns() - t0) / 1e9, h2 - h1,
	    count ? (double)(h2 - h1) / count : 0.0, (long long)(r2 - r1));
	print_load_stats("reload");
}

static void
cmd_sync(uint64_t n, int rounds)
{
	raidz_redirect_ent_t *e = malloc(n * sizeof (*e));
	uint64_t got = 0, done[2];

	/* New keys, spread over the vdev: see gen_other(). */
	for (uint64_t j = 0; j < universe && got < n; j++) {
		gen_other((j * 7919) % universe, &e[got]);
		if (e[got].rde_offset != 0)
			got++;
	}
	VERIFY3U(got, ==, n);
	for (int r = 0; r < rounds; r++) {
		uint64_t t0;

		/* An empty txg first, as the reference. */
		txg_wait_synced(spa_get_dsl(spa), 0);
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, 0,
		    SPIKE_SYNTH_ADD, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		(void) printf("MEASURE sync empty txg=%llu sync=%.6f s\n",
		    (u_longlong_t)done[1], sync_ns(done[1]) / 1e9);

		t0 = now_ns();
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, n,
		    SPIKE_SYNTH_ADD, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		(void) printf("MEASURE sync add n=%llu added=%llu txg=%llu "
		    "sync=%.3f s call=%.3f s table=%llu\n", (u_longlong_t)n,
		    (u_longlong_t)done[0], (u_longlong_t)done[1],
		    sync_ns(done[1]) / 1e9, (now_ns() - t0) / 1e9,
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
		VERIFY3U(done[0], ==, n);

		t0 = now_ns();
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, n,
		    SPIKE_SYNTH_REMOVE, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		(void) printf("MEASURE sync remove n=%llu removed=%llu "
		    "txg=%llu sync=%.3f s call=%.3f s table=%llu\n",
		    (u_longlong_t)n, (u_longlong_t)done[0],
		    (u_longlong_t)done[1], sync_ns(done[1]) / 1e9,
		    (now_ns() - t0) / 1e9,
		    (u_longlong_t)vdev_raidz_redirect_count(spa, 0));
		VERIFY3U(done[0], ==, n);
		/* Let the tombstones expire. */
		for (int i = 0; i < TXG_DEFER_SIZE + 2; i++) {
			VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, 0,
			    SPIKE_SYNTH_ADD, done));
			txg_wait_synced(spa_get_dsl(spa), done[1]);
		}
		punch_free();
		(void) fflush(stdout);
	}
	free(e);
}

/*
 * stress: lookups by reader threads while the sync thread changes the
 * shards they read.  Entry j < live_n is stable; churn key k is the odd
 * sector 2m sectors after entry j's, j = k mod live_n and m = 1 + k /
 * live_n, in j's metaslab and so in its shard: with more churn keys than
 * entries, each round grows every shard several times and shrinks it back.
 */
typedef struct srun {
	uint64_t	s_seed;
	uint64_t	s_churn;
	uint64_t	s_lookups;
	uint64_t	s_churn_found;
	uint64_t	s_missing;	/* a stable entry not found */
	uint64_t	s_wrong;	/* found, with other fields */
	uint64_t	s_first_bad;	/* first stable j missing or wrong */
} srun_t;

static volatile int stress_stop;

/* Whether e, as the table returned it, is x but for its new birth. */
static boolean_t
stress_same(const raidz_redirect_ent_t *e, const raidz_redirect_ent_t *x)
{
	return (e->rde_offset == x->rde_offset &&
	    e->rde_birth == x->rde_birth &&
	    e->rde_new_offset == x->rde_new_offset &&
	    e->rde_new_asize == x->rde_new_asize &&
	    e->rde_old_asize == x->rde_old_asize &&
	    e->rde_old_reserved == x->rde_old_reserved &&
	    e->rde_new_birth >= P.p_epoch && e->rde_new_birth > e->rde_birth);
}

static void
stress_other(uint64_t k, raidz_redirect_ent_t *e)
{
	uint64_t m = 1 + k / live_n;

	gen_entry(k % live_n, e);
	e->rde_offset += (2 * m) << P.p_ashift;
	e->rde_old_asize = 1ULL << P.p_ashift;
	if (m >= slot_pairs() || (e->rde_offset >> P.p_ms_shift) !=
	    ((e->rde_offset - ((2 * m) << P.p_ashift)) >> P.p_ms_shift))
		e->rde_offset = 0;
}

static void *
stress_thread(void *arg)
{
	srun_t *r = arg;
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	uint64_t s = r->s_seed;

	r->s_first_bad = UINT64_MAX;
	while (!stress_stop) {
		for (int i = 0; i < 1024; i++) {
			raidz_redirect_ent_t x, e;
			uint64_t j = mix(s++);

			if (j & 1) {
				j = (j >> 1) % live_n;
				gen_entry(j, &x);
				if (!vdev_raidz_redirect_get(vd, x.rde_offset,
				    x.rde_birth, &e)) {
					r->s_missing++;
				} else if (!stress_same(&e, &x)) {
					r->s_wrong++;
				} else {
					continue;
				}
				r->s_first_bad = MIN(r->s_first_bad, j);
			} else {
				stress_other((j >> 1) % r->s_churn, &x);
				if (x.rde_offset == 0 ||
				    !vdev_raidz_redirect_get(vd, x.rde_offset,
				    x.rde_birth, &e))
					continue;
				r->s_churn_found++;
				if (!stress_same(&e, &x))
					r->s_wrong++;
			}
		}
		r->s_lookups += 1024;
	}
	return (NULL);
}

static void
cmd_stress(int seconds, int readers, uint64_t churn)
{
	raidz_redirect_ent_t *e = malloc(churn * sizeof (*e));
	srun_t r[16];
	pthread_t tid[16];
	uint64_t got = 0, done[2], rounds = 0, t0, lookups = 0, found = 0;
	uint64_t missing = 0, wrong = 0, first = UINT64_MAX;
	uint64_t count0 = vdev_raidz_redirect_count(spa, 0);
	uint64_t add_ns = 0, remove_ns = 0, expire_ns = 0, t1;

	VERIFY3U(live_n, >, 0);
	VERIFY3S(readers, <=, 16);
	if (getenv("SCALE_UNSAFE") != NULL) {
		VERIFY3P(&spike_redirect_unsafe_readers, !=, NULL);
		spike_redirect_unsafe_readers = 1;
	}
	for (uint64_t k = 0; k < churn; k++) {
		stress_other(k, &e[got]);
		if (e[got].rde_offset != 0)
			got++;
	}
	(void) printf("INFO stress %d s, %d readers, %llu entries, %llu "
	    "churn keys, unsafe_readers=%d\n", seconds, readers,
	    (u_longlong_t)live_n, (u_longlong_t)got,
	    &spike_redirect_unsafe_readers != NULL ?
	    spike_redirect_unsafe_readers : -1);
	(void) fflush(stdout);
	stress_stop = 0;
	for (int t = 0; t < readers; t++) {
		memset(&r[t], 0, sizeof (r[t]));
		r[t].s_seed = 7777 + 1000003ULL * t;
		r[t].s_churn = churn;
		VERIFY0(pthread_create(&tid[t], NULL, stress_thread, &r[t]));
	}
	t0 = now_ns();
	while (now_ns() - t0 < seconds * 1000000000ULL) {
		t1 = now_ns();
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, got,
		    SPIKE_SYNTH_ADD_HOT, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		VERIFY3U(done[0], ==, got);
		add_ns += now_ns() - t1;
		t1 = now_ns();
		VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, got,
		    SPIKE_SYNTH_REMOVE, done));
		txg_wait_synced(spa_get_dsl(spa), done[1]);
		VERIFY3U(done[0], ==, got);
		remove_ns += now_ns() - t1;
		/* Let the tombstones expire: their slots are deleted. */
		t1 = now_ns();
		for (int i = 0; i < TXG_DEFER_SIZE + 1; i++) {
			VERIFY0(vdev_raidz_redirect_synth(spa, 0, e, 0,
			    SPIKE_SYNTH_ADD, done));
			txg_wait_synced(spa_get_dsl(spa), done[1]);
		}
		expire_ns += now_ns() - t1;
		rounds++;
	}
	stress_stop = 1;
	for (int t = 0; t < readers; t++) {
		VERIFY0(pthread_join(tid[t], NULL));
		lookups += r[t].s_lookups;
		found += r[t].s_churn_found;
		missing += r[t].s_missing;
		wrong += r[t].s_wrong;
		first = MIN(first, r[t].s_first_bad);
	}
	(void) printf("MEASURE stress rounds=%llu wall=%.1f s add=%.1f s "
	    "remove=%.1f s expire=%.1f s lookups=%llu "
	    "churn_found=%llu missing=%llu wrong=%llu table=%llu "
	    "(%llu before) condenses=%llu\n", (u_longlong_t)rounds,
	    (now_ns() - t0) / 1e9, add_ns / 1e9, remove_ns / 1e9,
	    expire_ns / 1e9, (u_longlong_t)lookups,
	    (u_longlong_t)found, (u_longlong_t)missing, (u_longlong_t)wrong,
	    (u_longlong_t)vdev_raidz_redirect_count(spa, 0),
	    (u_longlong_t)count0,
	    (u_longlong_t)(&spike_table_stats != NULL ?
	    spike_table_stats.sts_condense_calls : 0));
	if (missing != 0 || wrong != 0 || rounds == 0 || found == 0) {
		(void) printf("FAIL stress: %llu stable entries missed and "
		    "%llu wrong in %llu lookups (first entry %lld), %llu "
		    "rounds, %llu churn keys found\n",
		    (u_longlong_t)missing, (u_longlong_t)wrong,
		    (u_longlong_t)lookups,
		    first == UINT64_MAX ? -1LL : (long long)first,
		    (u_longlong_t)rounds, (u_longlong_t)found);
		exit(1);
	}
	(void) printf("PASS stress\n");
	if (&spike_redirect_unsafe_readers != NULL)
		spike_redirect_unsafe_readers = 0;
	free(e);
}

typedef struct rrun {
	objset_t	*rr_os;
	uint64_t	rr_obj;
	int		rr_err;
} rrun_t;

static void *
read_thread(void *arg)
{
	rrun_t *r = arg;
	void *buf = malloc(1 << 20);

	for (uint64_t off = 0; off < P.p_obj_size && r->rr_err == 0;
	    off += 1 << 20) {
		r->rr_err = dmu_read(r->rr_os, r->rr_obj, off,
		    MIN(1 << 20, P.p_obj_size - off), buf,
		    DMU_READ_NO_PREFETCH);
	}
	free(buf);
	return (NULL);
}

static void
cmd_read(int passes, int threads)
{
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	objset_t *os;

	VERIFY0(dmu_objset_own(POOL, DMU_OST_ANY, B_TRUE, B_TRUE, FTAG, &os));
	for (int p = 0; p < passes; p++) {
		rrun_t r[NOBJ];
		pthread_t tid[NOBJ];
		vdev_stat_t vs0, vs1;
		uint64_t t0, c0, h0 = 0, ios, blocks;
		double wall, cpu;
		int err = 0;

		vdev_get_stats(vd, &vs0);
		if (vdev_raidz_redirect_hits != NULL)
			h0 = vdev_raidz_redirect_hits();
		t0 = now_ns();
		c0 = cpu_ns();
		for (int i = 0; i < NOBJ; i += threads) {
			int m = MIN(threads, NOBJ - i);

			for (int t = 0; t < m; t++) {
				r[t].rr_os = os;
				r[t].rr_obj = P.p_obj[i + t];
				r[t].rr_err = 0;
				VERIFY0(pthread_create(&tid[t], NULL,
				    read_thread, &r[t]));
			}
			for (int t = 0; t < m; t++) {
				VERIFY0(pthread_join(tid[t], NULL));
				err = err != 0 ? err : r[t].rr_err;
			}
		}
		wall = (now_ns() - t0) / 1e9;
		cpu = (cpu_ns() - c0) / 1e9;
		vdev_get_stats(vd, &vs1);
		ios = vs1.vs_ops[ZIO_TYPE_READ] - vs0.vs_ops[ZIO_TYPE_READ];
		blocks = P.p_obj_size * NOBJ / RECSZ;
		(void) printf("MEASURE read pass=%d threads=%d bytes=%llu "
		    "blocks=%llu err=%d wall=%.3f s cpu=%.3f s leaf_reads=%llu "
		    "MB/s=%.1f wall_us_per_block=%.3f cpu_us_per_block=%.3f "
		    "redirect_hits=%llu\n", p, threads,
		    (u_longlong_t)(P.p_obj_size * NOBJ), (u_longlong_t)blocks,
		    err, wall, cpu, (u_longlong_t)ios,
		    P.p_obj_size * NOBJ / wall / 1e6, wall * 1e6 / blocks,
		    cpu * 1e6 / blocks,
		    (u_longlong_t)(vdev_raidz_redirect_hits != NULL ?
		    vdev_raidz_redirect_hits() - h0 : 0));
		(void) fflush(stdout);
	}
	dmu_objset_disown(os, B_TRUE, FTAG);
}

static void
object_report(const char *what, uint64_t obj, uint64_t entries)
{
	objset_t *mos = spa_meta_objset(spa);
	dmu_object_info_t doi;

	VERIFY0(dmu_object_info(mos, obj, &doi));
	(void) printf("MEASURE ondisk %s obj=%llu entries=%llu "
	    "logical=%llu allocated=%llu blksz=%u blocks=%llu "
	    "logical_per_entry=%.1f allocated_per_entry=%.1f\n", what,
	    (u_longlong_t)obj, (u_longlong_t)entries,
	    (u_longlong_t)doi.doi_max_offset,
	    (u_longlong_t)(doi.doi_physical_blocks_512 << 9),
	    doi.doi_data_block_size, (u_longlong_t)doi.doi_fill_count,
	    entries ? (double)doi.doi_max_offset / entries : 0.0,
	    entries ? (double)(doi.doi_physical_blocks_512 << 9) / entries :
	    0.0);
}

static void
cmd_ondisk(void)
{
	objset_t *mos = spa_meta_objset(spa);
	vdev_t *vd = spa->spa_root_vdev->vdev_child[0];
	uint64_t count = vdev_raidz_redirect_count(spa, 0);
	/* By name: the earlier format had one ZAP, this one a log and one. */
	static const char *const keys[] = {
	    "org.openzfs:spike_raidz_redirects",
	    "org.openzfs:spike_raidz_redirect_log",
	    "org.openzfs:spike_raidz_redirect_bps" };
	static const char *const what[] = { "zap", "log", "bps-zap" };
	uint64_t obj;

	for (int k = 0; k < 3; k++) {
		if (zap_lookup(mos, vd->vdev_top_zap, keys[k], sizeof (obj), 1,
		    &obj) != 0)
			continue;
		object_report(what[k], obj, count);
		if (k == 1) {
			dmu_buf_t *db;
			uint64_t hdr[2];

			VERIFY0(dmu_bonus_hold(mos, obj, FTAG, &db));
			memcpy(hdr, db->db_data, sizeof (hdr));
			dmu_buf_rele(db, FTAG);
			(void) printf("INFO log records=%llu sorted=%llu\n",
			    (u_longlong_t)hdr[0], (u_longlong_t)hdr[1]);
		} else {
			zap_stats_t zs;

			VERIFY0(zap_get_stats(mos, obj, &zs));
			(void) printf("INFO %s leafs=%llu entries=%llu "
			    "blocks=%llu ptrtbl=%llu\n", what[k],
			    (u_longlong_t)zs.zs_num_leafs,
			    (u_longlong_t)zs.zs_num_entries,
			    (u_longlong_t)zs.zs_num_blocks,
			    (u_longlong_t)zs.zs_ptrtbl_len);
		}
	}
}

#ifdef	RAIDZ_REDIRECT_REC_ADD
static void
dirty_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	vdev_dirty(spa->spa_root_vdev->vdev_child[0], 0, NULL,
	    dmu_tx_get_txg(tx));
}

/* Condense the log now: in the next txg, which dirties the vdev. */
static void
cmd_condense(void)
{
	uint64_t t0 = now_ns(), txg, c0;

	VERIFY3P(&spike_redirect_condense, !=, NULL);
	c0 = spike_table_stats.sts_condense_calls;
	spike_redirect_condense = 1;
	VERIFY0(dsl_sync_task(POOL, NULL, dirty_sync, NULL, 0,
	    ZFS_SPACE_CHECK_NONE));
	txg = spa_last_synced_txg(spa);
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("MEASURE condense calls=%llu txg=%llu records=%llu "
	    "condense=%.3f s txg sync=%.3f s call=%.3f s\n",
	    (u_longlong_t)(spike_table_stats.sts_condense_calls - c0),
	    (u_longlong_t)spike_table_stats.sts_condense_txg,
	    (u_longlong_t)spike_table_stats.sts_condense_records,
	    spike_table_stats.sts_condense_ns / 1e9,
	    sync_ns(spike_table_stats.sts_condense_txg) / 1e9,
	    (now_ns() - t0) / 1e9);
	(void) txg;
	punch_free();
}

#endif

static void
usage(void)
{
	(void) fprintf(stderr, "usage: scale_probe <dir> <ashift> <command> "
	    "[args] ...\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *e;
	int i;

	if (argc < 4)
		usage();
	dir = argv[1];
	ashift = strtoull(argv[2], NULL, 0);
	if ((e = getenv("SCALE_UNIVERSE")) != NULL)
		universe = strtoull(e, NULL, 0);
	{
		uint64_t a = PERM_A, b = universe;

		while (b != 0) {
			uint64_t t = a % b;

			a = b;
			b = t;
		}
		VERIFY3U(a, ==, 1);
	}
	if ((e = getenv("SCALE_FILE_GB")) != NULL)
		file_size = strtoull(e, NULL, 0) << 30;
	zfs_arc_max = (getenv("SCALE_ARC_MB") != NULL ?
	    strtoull(getenv("SCALE_ARC_MB"), NULL, 0) : 1024) << 20;
	/*
	 * Userland's arc_init() sets arc_c_min to half the default
	 * arc_c_max, and a zfs_arc_max no larger than arc_c_min is ignored.
	 */
	zfs_arc_min = MIN(zfs_arc_max / 2, 256ULL << 20);
#ifdef	RAIDZ_REDIRECT_REC_ADD
	if ((e = getenv("SCALE_CONDENSE")) != NULL) {
		VERIFY3P(&spike_redirect_condense, !=, NULL);
		spike_redirect_condense = atoi(e);
	}
#endif
	if (getenv("SCALE_LOAD_SKIP") != NULL) {
		VERIFY3P(&spike_redirect_load_skip, !=, NULL);
		spike_redirect_load_skip = 1;
	}
	(void) printf("INFO scale_probe dir=%s ashift=%llu file=%llu "
	    "universe=%llu arc_max=%llu load_skip=%d spike=%d\n", dir,
	    (u_longlong_t)ashift, (u_longlong_t)file_size,
	    (u_longlong_t)universe, (u_longlong_t)zfs_arc_max,
	    getenv("SCALE_LOAD_SKIP") != NULL,
	    vdev_raidz_redirect_synth != NULL);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	(void) printf("INFO arc c_min=%llu c_max=%llu\n",
	    (u_longlong_t)arc_c_min, (u_longlong_t)arc_c_max);
	for (i = 3; i < argc; i++) {
		const char *c = argv[i];

		(void) printf("INFO command %s\n", c);
		(void) fflush(stdout);
		if (strcmp(c, "create") == 0 && i + 1 < argc) {
			cmd_create(strtoull(argv[++i], NULL, 0));
		} else if (strcmp(c, "import") == 0 && i + 1 < argc) {
			const char *how = argv[++i];
			boolean_t cold = i + 1 < argc &&
			    strcmp(argv[i + 1], "cold") == 0;

			if (cold)
				i++;
			if (strcmp(how, "ro") == 0) {
				kernel_fini();
				kernel_init(SPA_MODE_READ);
			}
			cmd_import(how, cold);
		} else if (strcmp(c, "synth") == 0 && i + 4 < argc) {
			boolean_t sorted = i + 5 < argc &&
			    strcmp(argv[i + 5], "sorted") == 0;

			cmd_synth(strtoull(argv[i + 1], NULL, 0),
			    strtoull(argv[i + 2], NULL, 0), argv[i + 3],
			    strtoull(argv[i + 4], NULL, 0), sorted);
			i += sorted ? 5 : 4;
		} else if (strcmp(c, "lookup") == 0 && i + 1 < argc) {
			cmd_lookup(atoi(argv[++i]));
		} else if (strcmp(c, "memory") == 0) {
			boolean_t reload = i + 1 < argc &&
			    strcmp(argv[i + 1], "reload") == 0;

			if (reload)
				i++;
			cmd_memory(reload);
		} else if (strcmp(c, "sync") == 0 && i + 2 < argc) {
			cmd_sync(strtoull(argv[i + 1], NULL, 0),
			    atoi(argv[i + 2]));
			i += 2;
		} else if (strcmp(c, "read") == 0 && i + 2 < argc) {
			cmd_read(atoi(argv[i + 1]), atoi(argv[i + 2]));
			i += 2;
		} else if (strcmp(c, "stress") == 0 && i + 3 < argc) {
			cmd_stress(atoi(argv[i + 1]), atoi(argv[i + 2]),
			    strtoull(argv[i + 3], NULL, 0));
			i += 3;
		} else if (strcmp(c, "ondisk") == 0) {
			cmd_ondisk();
#ifdef	RAIDZ_REDIRECT_REC_ADD
		} else if (strcmp(c, "condense") == 0) {
			cmd_condense();
#endif
		} else if (strcmp(c, "export") == 0) {
			cmd_export();
		} else {
			usage();
		}
		(void) fflush(stdout);
	}
	kernel_fini();
	{
		char line[256];
		FILE *f = fopen("/proc/self/status", "r");

		while (f != NULL && fgets(line, sizeof (line), f) != NULL) {
			if (strncmp(line, "VmHWM", 5) == 0 ||
			    strncmp(line, "VmRSS", 5) == 0)
				(void) printf("INFO %s", line);
		}
		if (f != NULL)
			(void) fclose(f);
	}
	(void) printf("INFO done\n");
	return (0);
}
