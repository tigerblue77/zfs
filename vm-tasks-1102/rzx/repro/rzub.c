/*
 * Userspace reproduction, against libzpool, of an i/o that a RAID-Z
 * expansion maps with the stale reflow progress in spa_ubsync while
 * spa_sync() has already committed newer progress in spa_uberblock, and
 * that lands after the old location has been reused.
 *
 * usage: rzub DIR write|read [-n iterations] [-o tunable=value]...
 *
 *   write: in the window, rewrite in place, with zio_rewrite() as ZIL
 *          block writes, DDT repairs and corrective receive do, a block
 *          whose range was just committed (same content).
 *   read:  in the window, damage one column of such a block at its old
 *          location and read the block; the read reconstructs it and
 *          repairs the column at the old location.
 *
 * The window is held open by holding the first label write of a spa_sync()
 * whose spa_uberblock carries newer reflow progress than spa_ubsync.  The
 * old-location writes of that i/o are held (in a pwrite64() interposer) until
 * the reflow has copied other data over that location and synced it, unless
 * spa_ubsync does not advance while they are held.  Then the expansion
 * completes, the pool is exported, imported and scrubbed, and every block
 * written is read back and compared.
 *
 * Exit status: 0 no damage, 3 damage, 1 harness failure.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/uberblock_impl.h>
#include <sys/dmu.h>
#include <sys/dbuf.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/zio.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzpool.h>
#include <libspl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>
#include <pthread.h>

extern unsigned long raidz_expand_max_reflow_bytes;

#define	NOLD		4
#define	NDEVS		(NOLD + 1)
#define	ASHIFT		12
#define	SECTOR		(1ULL << ASHIFT)
#define	DEV_SIZE	(256ULL << 20)
#define	POOL		"rzub"
#define	BIGBS		(128 << 10)
#define	SMALLSZ		(3 * SECTOR)	/* one row of a 4-wide raidz1 */
#define	MAXCAND		4096

static const char *dir;

/* pwrite64() interposer */
static ssize_t (*real_pwrite64)(int, const void *, size_t, off64_t);
static pthread_mutex_t ip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ip_cv = PTHREAD_COND_INITIALIZER;
static volatile int ip_active;		/* any hold armed or held */
static int ip_label_armed;
static int ip_label_held;
static int ip_capture;
static uint64_t ip_tgt_off[NDEVS];	/* file offset, 0 = none */
static uint64_t ip_tgt_len[NDEVS];
static int ip_ncaptured;
static int ip_nheld;
static int ip_release;

static int
fd_child(int fd)
{
	char p[64], buf[MAXPATHLEN];
	(void) snprintf(p, sizeof (p), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(p, buf, sizeof (buf) - 1);
	if (n <= 0)
		return (-1);
	buf[n] = '\0';
	const char *b = strrchr(buf, '/');
	if (b == NULL || strncmp(b + 1, "dev-", 4) != 0)
		return (-1);
	int k = atoi(b + 5);
	return (k >= 1 && k <= NDEVS ? k - 1 : -1);
}

ssize_t
pwrite64(int fd, const void *buf, size_t count, off64_t off)
{
	if (real_pwrite64 == NULL)
		real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
	if (ip_active && count > 0) {
		int c = fd_child(fd);
		pthread_mutex_lock(&ip_lock);
		if (c >= 0 && ip_label_armed &&
		    (off < (512 << 10) || off >= DEV_SIZE - (512 << 10))) {
			ip_label_armed = 0;
			ip_label_held = 1;
			pthread_cond_broadcast(&ip_cv);
			while (ip_label_held)
				pthread_cond_wait(&ip_cv, &ip_lock);
		} else if (c >= 0 && ip_capture && ip_tgt_len[c] != 0 &&
		    (uint64_t)off < ip_tgt_off[c] + ip_tgt_len[c] &&
		    (uint64_t)off + count > ip_tgt_off[c]) {
			ip_ncaptured++;
			ip_nheld++;
			(void) fprintf(stderr, "held write: child %d off %llu "
			    "len %zu\n", c, (u_longlong_t)off, count);
			pthread_cond_broadcast(&ip_cv);
			while (!ip_release)
				pthread_cond_wait(&ip_cv, &ip_lock);
			ip_nheld--;
		}
		pthread_mutex_unlock(&ip_lock);
	}
	return (real_pwrite64(fd, buf, count, off));
}

static void
nap(int ms)
{
	(void) poll(NULL, 0, ms);
}

/* Wait up to ms for *flag to become nonzero, under ip_lock. */
static int
ip_wait(volatile int *flag, int want, int ms)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&ip_lock);
	while (*flag < want) {
		if (pthread_cond_timedwait(&ip_cv, &ip_lock, &ts) != 0)
			break;
	}
	int r = (*flag >= want);
	pthread_mutex_unlock(&ip_lock);
	return (r);
}

static void
ip_release_label(void)
{
	pthread_mutex_lock(&ip_lock);
	ip_label_armed = 0;
	ip_label_held = 0;
	pthread_cond_broadcast(&ip_cv);
	pthread_mutex_unlock(&ip_lock);
}

static void
fill(uint8_t *buf, size_t len, uint64_t seed)
{
	uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 0x1234567;
	for (size_t i = 0; i + 8 <= len; i += 8) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		memcpy(buf + i, &x, 8);
	}
}

static nvlist_t *
mkfile_nv(int i)
{
	char path[MAXPATHLEN];
	(void) snprintf(path, sizeof (path), "%s/dev-%d", dir, i);
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, DEV_SIZE));
	(void) close(fd);

	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_ASHIFT, ASHIFT);
	return (nv);
}

static nvlist_t *
mkroot(nvlist_t **child, int n)
{
	nvlist_t *root = fnvlist_alloc();
	fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)child, n);
	return (root);
}

static uint64_t
new_object(objset_t *os, uint64_t bs)
{
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	uint64_t obj = dmu_object_alloc(os, DMU_OT_UINT64_OTHER, bs,
	    DMU_OT_NONE, 0, tx);
	dmu_tx_commit(tx);
	return (obj);
}

static void
write_blocks(objset_t *os, uint64_t obj, uint64_t bs, uint64_t off,
    uint64_t len, uint8_t *buf)
{
	for (uint64_t o = off; o < off + len; o += bs)
		fill(buf + (o - off), bs, obj * 1000003 + o / bs);
	dmu_tx_t *tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, obj, off, len);
	VERIFY0(dmu_tx_assign(tx, DMU_TX_WAIT));
	dmu_write(os, obj, off, len, buf, tx, 0);
	dmu_tx_commit(tx);
}

/* Read back and compare; returns the number of bad blocks. */
static int
verify_blocks(objset_t *os, uint64_t obj, uint64_t bs, uint64_t len,
    uint8_t *buf, uint8_t *exp, int *neio)
{
	int bad = 0;
	for (uint64_t o = 0; o < len; o += bs) {
		int err = dmu_read(os, obj, o, bs, buf, DMU_READ_NO_PREFETCH);
		fill(exp, bs, obj * 1000003 + o / bs);
		if (err == 0 && memcmp(buf, exp, bs) == 0)
			continue;
		bad++;
		if (err != 0)
			(*neio)++;
		dmu_buf_t *db;
		uint64_t dva = UINT64_MAX, asize = 0;
		if (dmu_buf_hold_noread(os, obj, o, FTAG, &db) == 0) {
			blkptr_t *bp = ((dmu_buf_impl_t *)db)->db_blkptr;
			if (bp != NULL) {
				dva = DVA_GET_OFFSET(&bp->blk_dva[0]);
				asize = DVA_GET_ASIZE(&bp->blk_dva[0]);
			}
			dmu_buf_rele(db, FTAG);
		}
		(void) printf("bad block: object %llu offset %llu (%s), DVA "
		    "offset %llu, sectors %llu-%llu\n", (u_longlong_t)obj,
		    (u_longlong_t)o, err != 0 ? "EIO" : "wrong content",
		    (u_longlong_t)dva, (u_longlong_t)(dva >> ASHIFT),
		    (u_longlong_t)((dva + asize) >> ASHIFT) - 1);
	}
	return (bad);
}

/*
 * A write held in pwrite64() stays active in its leaf's vdev queue, and
 * while an interactive i/o is active, vdev_queue lets non-interactive i/o
 * (the reflow's ZIO_PRIORITY_REMOVAL copies) through only on the credit that
 * completing interactive i/o gives back.  Keep small interactive reads
 * going on every leaf while writes are held, as other load on the pool
 * would, so that the reflow is not starved by the held write itself.
 */
static spa_t *tick_spa;
static volatile int tick_on;

static void *
ticker(void *arg)
{
	(void) arg;
	abd_t *abd[NDEVS];
	for (int i = 0; i < NDEVS; i++)
		abd[i] = abd_alloc_linear(SECTOR, B_FALSE);
	while (tick_on) {
		spa_config_enter(tick_spa, SCL_STATE, FTAG, RW_READER);
		vdev_t *rz = tick_spa->spa_root_vdev->vdev_child[0];
		zio_t *pio = zio_root(tick_spa, NULL, NULL, ZIO_FLAG_CANFAIL);
		for (int i = 0; i < rz->vdev_children && i < NDEVS; i++) {
			zio_nowait(zio_read_phys(pio, rz->vdev_child[i],
			    VDEV_LABEL_START_SIZE, SECTOR, abd[i],
			    ZIO_CHECKSUM_OFF, NULL, NULL,
			    ZIO_PRIORITY_SYNC_READ,
			    ZIO_FLAG_CANFAIL | ZIO_FLAG_DONT_RETRY, B_FALSE));
		}
		(void) zio_wait(pio);
		spa_config_exit(tick_spa, SCL_STATE, FTAG);
		nap(1);
	}
	for (int i = 0; i < NDEVS; i++)
		abd_free(abd[i]);
	return (NULL);
}

typedef struct cand {
	uint64_t obj;
	blkptr_t bp;
	uint64_t off;		/* DVA offset on the raidz vdev */
	uint8_t raw[SMALLSZ];	/* on-disk content */
} cand_t;

static cand_t *cands;
static int ncand;

static boolean_t
expanding(spa_t *spa)
{
	vdev_raidz_expand_t *vre = spa->spa_raidz_expand;
	return (vre != NULL && vre->vre_state == DSS_SCANNING);
}

static uint64_t
ubsync_offset(spa_t *spa)
{
	return (RRSS_GET_OFFSET(&spa->spa_ubsync));
}

/* Sector x of the vdev in the old (NOLD-wide) layout: child and row. */
static void
old_loc(uint64_t x, int *child, uint64_t *row)
{
	*child = x % NOLD;
	*row = x / NOLD;
}

int
main(int argc, char **argv)
{
	int iters = 160, c;
	boolean_t wmode;
	nvlist_t *child[NDEVS];
	char dsname[ZFS_MAX_DATASET_NAME_LEN];
	spa_t *spa;
	objset_t *os;

	if (argc < 3) {
		(void) fprintf(stderr, "usage: rzub DIR write|read [-n N] "
		    "[-o tunable=value]...\n");
		return (1);
	}
	dir = argv[1];
	wmode = (strcmp(argv[2], "write") == 0);
	if (!wmode && strcmp(argv[2], "read") != 0)
		return (1);
	optind = 3;

	real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	libspl_init();
	random_force_pseudo(B_TRUE);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	VERIFY0(handle_tunable_option("zfs_scrub_after_expand=0", B_FALSE));
	while ((c = getopt(argc, argv, "n:o:")) != -1) {
		switch (c) {
		case 'n': iters = atoi(optarg); break;
		case 'o':
			if (handle_tunable_option(optarg, B_FALSE) != 0)
				return (1);
			break;
		default: return (1);
		}
	}

	uint8_t *buf = kmem_alloc(1 << 20, KM_SLEEP);
	uint8_t *exp = kmem_alloc(1 << 20, KM_SLEEP);
	cands = kmem_zalloc(sizeof (cand_t) * MAXCAND, KM_SLEEP);

	(void) spa_destroy(POOL);
	for (int i = 0; i < NDEVS; i++)
		child[i] = mkfile_nv(i + 1);
	nvlist_t *raidz = fnvlist_alloc();
	fnvlist_add_string(raidz, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
	fnvlist_add_uint64(raidz, ZPOOL_CONFIG_NPARITY, 1);
	fnvlist_add_nvlist_array(raidz, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)child, NOLD);
	nvlist_t *root = mkroot(&raidz, 1);
	nvlist_t *props = fnvlist_alloc();
	for (int i = 0; i < SPA_FEATURES; i++) {
		char *fn;
		VERIFY3S(-1, !=, asprintf(&fn, "feature@%s",
		    spa_feature_table[i].fi_uname));
		fnvlist_add_uint64(props, fn, 0);
		free(fn);
	}
	VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root);
	fnvlist_free(raidz);
	VERIFY0(spa_open(POOL, &spa, FTAG));

	(void) snprintf(dsname, sizeof (dsname), "%s/ds", POOL);
	VERIFY0(dmu_objset_create(dsname, DMU_OST_OTHER, 0, NULL, NULL,
	    NULL));
	VERIFY0(dmu_objset_own(dsname, DMU_OST_OTHER, B_FALSE, B_TRUE, FTAG,
	    &os));

	/* Big blocks interleaved with one-row blocks. */
	uint64_t big = new_object(os, BIGBS);
	uint64_t *small = kmem_zalloc(sizeof (uint64_t) * iters, KM_SLEEP);
	for (int i = 0; i < iters; i++) {
		write_blocks(os, big, BIGBS, (uint64_t)i << 20, 1 << 20, buf);
		small[i] = new_object(os, SMALLSZ);
		write_blocks(os, small[i], SMALLSZ, 0, SMALLSZ, buf);
		if (i % 16 == 15)
			txg_wait_synced(spa_get_dsl(spa), 0);
	}
	txg_wait_synced(spa_get_dsl(spa), 0);

	for (int i = 0; i < iters && ncand < MAXCAND; i++) {
		dmu_buf_t *db;
		VERIFY0(dmu_buf_hold(os, small[i], 0, FTAG, &db,
		    DMU_READ_NO_PREFETCH));
		blkptr_t *bp = ((dmu_buf_impl_t *)db)->db_blkptr;
		if (bp != NULL && !BP_IS_EMBEDDED(bp) && !BP_IS_GANG(bp) &&
		    BP_GET_PSIZE(bp) == SMALLSZ &&
		    DVA_GET_ASIZE(&bp->blk_dva[0]) == NOLD * SECTOR) {
			cand_t *cd = &cands[ncand++];
			cd->obj = small[i];
			cd->bp = *bp;
			cd->off = DVA_GET_OFFSET(&bp->blk_dva[0]);
		}
		dmu_buf_rele(db, FTAG);
	}
	for (int i = 0; i < ncand; i++) {
		zbookmark_phys_t zb;
		SET_BOOKMARK(&zb, dmu_objset_id(os), cands[i].obj, 0, 0);
		abd_t *abd = abd_alloc_linear(SMALLSZ, B_FALSE);
		VERIFY0(zio_wait(zio_read(NULL, spa, &cands[i].bp, abd,
		    SMALLSZ, NULL, NULL, ZIO_PRIORITY_SYNC_READ,
		    ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW, &zb)));
		abd_copy_to_buf(cands[i].raw, abd, SMALLSZ);
		abd_free(abd);
	}
	(void) printf("wrote %d MiB in 128K blocks and %d one-row blocks "
	    "(%d usable)\n", iters, iters, ncand);

	/* Expand. */
	vdev_t *rzvd = spa->spa_root_vdev->vdev_child[0];
	nvlist_t *aroot = mkroot(&child[NDEVS - 1], 1);
	VERIFY0(spa_vdev_attach(spa, rzvd->vdev_guid, aroot, B_FALSE,
	    B_FALSE));
	fnvlist_free(aroot);
	for (int i = 0; i < NDEVS; i++)
		fnvlist_free(child[i]);
	while (!expanding(spa))
		nap(10);
	while (expanding(spa) && (ubsync_offset(spa) == 0 ||
	    RRSS_GET_STATE(&spa->spa_ubsync) == RRSS_SCRATCH_VALID))
		nap(10);

	/* Find a spa_sync() whose committed range holds a candidate. */
	cand_t *b = NULL;
	uint64_t old_off = 0, new_off = 0, wtxg = 0;
	for (int tries = 0; tries < 2000 && b == NULL && expanding(spa);
	    tries++) {
		pthread_mutex_lock(&ip_lock);
		ip_label_armed = 1;
		ip_active = 1;
		pthread_mutex_unlock(&ip_lock);
		if (!ip_wait(&ip_label_held, 1, 30000)) {
			ip_release_label();
			continue;
		}
		old_off = RRSS_GET_OFFSET(&spa->spa_ubsync);
		new_off = RRSS_GET_OFFSET(&spa->spa_uberblock);
		wtxg = spa->spa_uberblock.ub_txg;
		if (new_off > old_off &&
		    RRSS_GET_STATE(&spa->spa_uberblock) ==
		    RRSS_SCRATCH_INVALID_SYNCED_REFLOW) {
			for (int i = 0; i < ncand; i++) {
				if (cands[i].off >= old_off &&
				    cands[i].off + NOLD * SECTOR <= new_off) {
					b = &cands[i];
					break;
				}
			}
		}
		if (b == NULL) {
			ip_release_label();
			while (spa->spa_ubsync.ub_txg < wtxg && expanding(spa))
				nap(1);
		}
	}
	if (b == NULL) {
		(void) printf("no window holding a candidate block\n");
		return (1);
	}

	uint64_t bs = b->off >> ASHIFT;
	(void) printf("window: txg %llu, spa_ubsync offset %llu, "
	    "spa_uberblock offset %llu, vre_offset %llu\n",
	    (u_longlong_t)wtxg, (u_longlong_t)old_off,
	    (u_longlong_t)new_off,
	    (u_longlong_t)spa->spa_raidz_expand->vre_offset);
	(void) printf("block: object %llu, DVA offset %llu (sector %llu)\n",
	    (u_longlong_t)b->obj, (u_longlong_t)b->off, (u_longlong_t)bs);

	/* Old-location sectors to hold, and the new sectors they overlay. */
	uint64_t smax = 0;
	int dchild = -1;
	for (int k = 0; k < NOLD; k++) {
		int ch;
		uint64_t row;
		old_loc(bs + k, &ch, &row);
		uint64_t s = row * NDEVS + ch;
		smax = MAX(smax, s);
		(void) printf("  sector %llu: old location child %d row %llu, "
		    "which is the new location of sector %llu\n",
		    (u_longlong_t)(bs + k), ch, (u_longlong_t)row,
		    (u_longlong_t)s);
		if (wmode || k == 2) {
			ip_tgt_off[ch] = (row << ASHIFT) +
			    VDEV_LABEL_START_SIZE;
			ip_tgt_len[ch] = SECTOR;
			if (!wmode)
				dchild = ch;
		}
	}
	int want = wmode ? NOLD : 1;

	pthread_t tick;
	tick_spa = spa;
	tick_on = 1;
	VERIFY0(pthread_create(&tick, NULL, ticker, NULL));

	zio_t *rio = zio_root(spa, NULL, NULL, ZIO_FLAG_CANFAIL);
	abd_t *abd = abd_alloc_linear(SMALLSZ, B_FALSE);
	blkptr_t bpc = b->bp;
	zbookmark_phys_t zb;
	SET_BOOKMARK(&zb, dmu_objset_id(os), b->obj, 0, 0);
	pthread_mutex_lock(&ip_lock);
	ip_capture = 1;
	pthread_mutex_unlock(&ip_lock);
	spa_config_enter(spa, SCL_STATE, FTAG, RW_READER);
	if (wmode) {
		abd_copy_from_buf(abd, b->raw, SMALLSZ);
		zio_nowait(zio_rewrite(rio, spa, 0, &bpc, abd, SMALLSZ, NULL,
		    NULL, ZIO_PRIORITY_ASYNC_WRITE, ZIO_FLAG_CANFAIL, &zb));
	} else {
		char path[MAXPATHLEN];
		uint8_t junk[SECTOR];
		(void) memset(junk, 0xa5, sizeof (junk));
		(void) snprintf(path, sizeof (path), "%s/dev-%d", dir,
		    dchild + 1);
		int fd = open(path, O_RDWR);
		VERIFY3S(fd, >=, 0);
		VERIFY3S(real_pwrite64(fd, junk, SECTOR, ip_tgt_off[dchild]),
		    ==, SECTOR);
		(void) close(fd);
		(void) printf("damaged child %d at its old location\n",
		    dchild);
		zio_nowait(zio_read(rio, spa, &bpc, abd, SMALLSZ, NULL, NULL,
		    ZIO_PRIORITY_SYNC_READ, ZIO_FLAG_CANFAIL | ZIO_FLAG_RAW,
		    &zb));
	}
	int got = ip_wait(&ip_ncaptured, want, 10000);
	pthread_mutex_lock(&ip_lock);
	ip_capture = 0;
	int held = ip_nheld;
	pthread_mutex_unlock(&ip_lock);
	(void) printf("old-location writes held: %d of %d\n", held, want);
	if (!got && held == 0) {
		(void) printf("no old-location write was issued\n");
	}

	/* Let spa_sync() finish. */
	ip_release_label();
	hrtime_t t0 = gethrtime();
	while (spa->spa_ubsync.ub_txg < wtxg &&
	    gethrtime() - t0 < SEC2NSEC(10))
		nap(1);
	int verdict_window = 0;
	if (spa->spa_ubsync.ub_txg < wtxg) {
		(void) printf("spa_ubsync stays at txg %llu while the write is "
		    "in flight\n", (u_longlong_t)spa->spa_ubsync.ub_txg);
	} else {
		verdict_window = 1;
		uint64_t target = (smax + 2 * NDEVS + 1) << ASHIFT;
		(void) printf("spa_ubsync advanced to offset %llu with the "
		    "write in flight; waiting for it to pass %llu\n",
		    (u_longlong_t)ubsync_offset(spa), (u_longlong_t)target);
		while (expanding(spa) && ubsync_offset(spa) < target)
			nap(1);
		(void) printf("spa_ubsync offset %llu: the reflow has copied "
		    "and synced the new location of sectors up to %llu\n",
		    (u_longlong_t)ubsync_offset(spa), (u_longlong_t)smax);
	}
	pthread_mutex_lock(&ip_lock);
	ip_release = 1;
	pthread_cond_broadcast(&ip_cv);
	pthread_mutex_unlock(&ip_lock);
	int zerr = zio_wait(rio);
	spa_config_exit(spa, SCL_STATE, FTAG);
	tick_on = 0;
	VERIFY0(pthread_join(tick, NULL));
	abd_free(abd);
	(void) printf("held writes released, i/o done (error %d)%s\n", zerr,
	    verdict_window ? ", after the reflow overwrote their location" :
	    "");
	if (spa->spa_ubsync.ub_txg < wtxg) {
		while (spa->spa_ubsync.ub_txg < wtxg)
			nap(1);
		(void) printf("spa_ubsync advanced once the write was done "
		    "(%.1f s after the window)\n",
		    (double)(gethrtime() - t0) / NANOSEC);
	}
	ip_active = 0;

	while (expanding(spa))
		nap(50);
	txg_wait_synced(spa_get_dsl(spa), 0);
	(void) printf("expansion complete\n");

	dmu_objset_disown(os, B_TRUE, FTAG);
	spa_close(spa, FTAG);
	nvlist_t *config;
	VERIFY0(spa_export(POOL, &config, B_FALSE, B_FALSE));
	VERIFY0(spa_import(POOL, config, NULL, 0));
	nvlist_free(config);
	VERIFY0(spa_open(POOL, &spa, FTAG));

	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	while (dsl_scan_scrubbing(spa_get_dsl(spa)))
		txg_wait_synced(spa_get_dsl(spa), 0);
	dsl_scan_t *scn = spa_get_dsl(spa)->dp_scan;
	uint64_t cksum = 0;
	rzvd = spa->spa_root_vdev->vdev_child[0];
	(void) printf("scrub: %llu errors, %llu bytes repaired, "
	    "%llu permanent error log entries; checksum errors per child:",
	    (u_longlong_t)scn->scn_phys.scn_errors,
	    (u_longlong_t)scn->scn_phys.scn_processed,
	    (u_longlong_t)spa_approx_errlog_size(spa));
	for (int i = 0; i < rzvd->vdev_children; i++) {
		vdev_stat_t *vs = &rzvd->vdev_child[i]->vdev_stat;
		cksum += vs->vs_checksum_errors;
		(void) printf(" %llu", (u_longlong_t)vs->vs_checksum_errors);
	}
	(void) printf("\n");
	uint64_t errlog = spa_approx_errlog_size(spa);
	uint64_t scnerr = scn->scn_phys.scn_errors;

	VERIFY0(dmu_objset_own(dsname, DMU_OST_OTHER, B_TRUE, B_TRUE, FTAG,
	    &os));
	int neio = 0;
	int bad = verify_blocks(os, big, BIGBS, (uint64_t)iters << 20, buf,
	    exp, &neio);
	for (int i = 0; i < iters; i++)
		bad += verify_blocks(os, small[i], SMALLSZ, SMALLSZ, buf, exp,
		    &neio);
	(void) printf("read back: %d bad blocks (%d EIO) of %d\n", bad, neio,
	    iters * 9);
	dmu_objset_disown(os, B_TRUE, FTAG);
	spa_close(spa, FTAG);
	VERIFY0(spa_destroy(POOL));
	kernel_fini();

	boolean_t damaged = (bad != 0 || cksum != 0 || errlog != 0 ||
	    scnerr != 0);
	(void) printf("RESULT: %s\n", damaged ? "DAMAGED" : "CLEAN");
	return (damaged ? 3 : 0);
}
