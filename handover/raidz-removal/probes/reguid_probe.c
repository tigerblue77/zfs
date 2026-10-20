/*
 * Scratch probe (not for upstream): userspace libzpool, file vdevs.
 * Creates a pool (mirror vd.0 vd.1, file vd.2), syncs, copies the devices to
 * <dir>/pre, then changes the pool guid to NEWGUID and exports.  Run under
 * gdb with a breakpoint in the label or uberblock writes of the reguid txg,
 * then kill, to leave the exact state of a crash at that point.
 *
 * Usage: reguid_probe <scratch-dir> [export]
 * With "export", the copy is taken from the exported pool, which is then
 * imported again before the reguid.  "export noreguid" skips the reguid.
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/dsl_pool.h>
#include <sys/txg.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define	NEWGUID	0x0123456789abcdefULL

static char dir[1024];
static char pool[] = "rg";

static nvlist_t *
mk_file(int i)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, i);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 128ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static nvlist_t *
mk_vdev(const char *type, nvlist_t **c, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, type);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	return (r);
}

static void
copy_file(const char *from, const char *to)
{
	char buf[1 << 16];
	int in = open(from, O_RDONLY);
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ssize_t n;

	VERIFY3S(in, >=, 0);
	VERIFY3S(out, >=, 0);
	while ((n = read(in, buf, sizeof (buf))) > 0)
		VERIFY3S(write(out, buf, n), ==, n);
	VERIFY0(n);
	(void) close(in);
	(void) close(out);
}

int
main(int argc, char **argv)
{
	char cache[1200], pre[1200];
	spa_t *spa;

	if (argc < 2)
		return (2);
	(void) strlcpy(dir, argv[1], sizeof (dir));
	(void) snprintf(cache, sizeof (cache), "%s/zpool.cache", dir);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *m[2] = { mk_file(0), mk_file(1) };
	nvlist_t *t[2] = { mk_vdev(VDEV_TYPE_MIRROR, m, 2), mk_file(2) };
	nvlist_t *rt = mk_vdev(VDEV_TYPE_ROOT, t, 2);
	VERIFY0(spa_create(pool, rt, NULL, NULL, NULL, NULL));
	fnvlist_free(rt);

	boolean_t viaexport = argc > 2 && strcmp(argv[2], "export") == 0;
	nvlist_t *ecfg = NULL;
	if (viaexport)
		VERIFY0(spa_export(pool, &ecfg, B_FALSE, B_FALSE));
	else
		VERIFY0(spa_open(pool, &spa, FTAG));
	if (!viaexport)
		txg_wait_synced(spa_get_dsl(spa), 0);
	if (!viaexport)
		(void) printf("old guid %llu, txg %llu\n",
		    (u_longlong_t)spa_guid(spa),
		    (u_longlong_t)spa_last_synced_txg(spa));
	(void) snprintf(pre, sizeof (pre), "%s/pre", dir);
	VERIFY0(mkdir(pre, 0755));
	for (int i = 0; i < 3; i++) {
		char a[1200], b[1300];

		(void) snprintf(a, sizeof (a), "%s/vd.%d", dir, i);
		(void) snprintf(b, sizeof (b), "%s/vd.%d", pre, i);
		copy_file(a, b);
	}
	(void) fflush(stdout);

	if (viaexport) {
		VERIFY0(spa_import(pool, ecfg, NULL, 0));
		fnvlist_free(ecfg);
		VERIFY0(spa_open(pool, &spa, FTAG));
		(void) printf("old guid %llu\n", (u_longlong_t)spa_guid(spa));
	}
	uint64_t ng = NEWGUID;
	if (argc > 3 && strcmp(argv[3], "noreguid") == 0)
		txg_wait_synced(spa_get_dsl(spa), 0);
	else
		VERIFY0(spa_change_guid(spa, &ng));
	(void) printf("new guid %llu, txg %llu\n",
	    (u_longlong_t)spa_guid(spa),
	    (u_longlong_t)spa_last_synced_txg(spa));
	spa_close(spa, FTAG);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (0);
}
