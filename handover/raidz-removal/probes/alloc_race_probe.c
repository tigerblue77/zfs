/*
 * Scratch probe (not for upstream): two threads set allocating=on on the same
 * non-allocating vdev at the same time, through vdev_prop_set() as the
 * ZFS_IOC_VDEV_SET_PROPS ioctl does.  vdev_prop_set() checks vdev_noalloc
 * under SCL_CONFIG, drops it, and spa_vdev_alloc() activates the vdev
 * without checking again.
 *
 * Usage: alloc_race_probe <scratch-dir> <iterations>
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/metaslab_impl.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static const char *pool = "racepool";
static uint64_t guid;
static pthread_barrier_t bar;

static nvlist_t *
mk_file(const char *dir, int i)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, i);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, 256ULL << 20));
	(void) close(fd);
	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, 9);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, 0);
	return (f);
}

static int
set_allocating(uint64_t on)
{
	spa_t *spa;
	nvlist_t *in = fnvlist_alloc(), *props = fnvlist_alloc();
	nvlist_t *out = fnvlist_alloc();

	fnvlist_add_uint64(props, vdev_prop_to_name(VDEV_PROP_ALLOCATING), on);
	fnvlist_add_uint64(in, ZPOOL_VDEV_PROPS_SET_VDEV, guid);
	fnvlist_add_nvlist(in, ZPOOL_VDEV_PROPS_SET_PROPS, props);
	VERIFY0(spa_open(pool, &spa, FTAG));
	int e = vdev_prop_set(spa, in, out);
	spa_close(spa, FTAG);
	fnvlist_free(in);
	fnvlist_free(props);
	fnvlist_free(out);
	return (e);
}

static void *
racer(void *arg)
{
	(void) arg;
	(void) pthread_barrier_wait(&bar);
	(void) set_allocating(1);
	return (NULL);
}

int
main(int argc, char **argv)
{
	spa_t *spa;
	int iters;

	if (argc < 3)
		return (2);
	iters = atoi(argv[2]);
	(void) setvbuf(stdout, NULL, _IOLBF, 0);
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

	nvlist_t *tops[2] = { mk_file(argv[1], 0), mk_file(argv[1], 1) };
	nvlist_t *rt = fnvlist_alloc();
	fnvlist_add_string(rt, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(rt, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)tops, 2);
	VERIFY0(spa_create(pool, rt, NULL, NULL, NULL, NULL));
	VERIFY0(spa_open(pool, &spa, FTAG));
	guid = spa->spa_root_vdev->vdev_child[1]->vdev_guid;
	spa_close(spa, FTAG);

	for (int i = 0; i < iters; i++) {
		pthread_t t1, t2;

		VERIFY0(set_allocating(0));
		VERIFY0(pthread_barrier_init(&bar, NULL, 2));
		VERIFY0(pthread_create(&t1, NULL, racer, NULL));
		VERIFY0(pthread_create(&t2, NULL, racer, NULL));
		VERIFY0(pthread_join(t1, NULL));
		VERIFY0(pthread_join(t2, NULL));
		VERIFY0(pthread_barrier_destroy(&bar));

		VERIFY0(spa_open(pool, &spa, FTAG));
		vdev_t *vd = spa->spa_root_vdev->vdev_child[1];
		int64_t ac = vd->vdev_mg->mg_activation_count;
		uint64_t na = spa->spa_nonallocating_dspace;
		spa_close(spa, FTAG);
		if (ac != 1 || na != 0) {
			(void) printf("iteration %d: activation_count %lld, "
			    "nonallocating %llu\n", i, (long long)ac,
			    (u_longlong_t)na);
			return (1);
		}
	}
	(void) printf("no double activation in %d iterations\n", iters);
	VERIFY0(spa_export(pool, NULL, B_FALSE, B_FALSE));
	kernel_fini();
	return (0);
}
