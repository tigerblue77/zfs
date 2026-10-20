/*
 * Scratch probe (not for upstream): the steps of the detach ZTS test with
 * libzpool on file vdevs.
 *
 * Usage: detpath_zts_probe <dir> create    mirror(vdev0, vdev1), export
 *        detpath_zts_probe <dir> detach    import from a scan of <dir>,
 *                                          detach vdev1, export
 */
#include <libzpool.h>
#include <libzutil.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static vdev_t *
find_suffix(vdev_t *vd, const char *sfx)
{
	if (vd->vdev_path != NULL && strlen(vd->vdev_path) >= strlen(sfx) &&
	    strcmp(vd->vdev_path + strlen(vd->vdev_path) - strlen(sfx),
	    sfx) == 0)
		return (vd);
	for (uint64_t c = 0; c < vd->vdev_children; c++) {
		vdev_t *f = find_suffix(vd->vdev_child[c], sfx);
		if (f != NULL)
			return (f);
	}
	return (NULL);
}

int
main(int argc, char **argv)
{
	char cache[1200], p[2][1100];
	spa_t *spa;

	if (argc < 3)
		return (2);
	(void) snprintf(cache, sizeof (cache), "%s/zts.cache", argv[1]);
	spa_config_path = cache;
	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	if (strcmp(argv[2], "create") == 0) {
		nvlist_t *f[2];
		for (int i = 0; i < 2; i++) {
			(void) snprintf(p[i], sizeof (p[i]), "%s/vdev%d",
			    argv[1], i);
			int fd = open(p[i], O_RDWR | O_CREAT | O_TRUNC, 0666);
			VERIFY0(ftruncate(fd, 256 << 20));
			(void) close(fd);
			f[i] = fnvlist_alloc();
			fnvlist_add_string(f[i], ZPOOL_CONFIG_TYPE,
			    VDEV_TYPE_FILE);
			fnvlist_add_string(f[i], ZPOOL_CONFIG_PATH, p[i]);
			fnvlist_add_uint64(f[i], ZPOOL_CONFIG_IS_LOG, 0);
		}
		nvlist_t *m = fnvlist_alloc();
		fnvlist_add_string(m, ZPOOL_CONFIG_TYPE, VDEV_TYPE_MIRROR);
		fnvlist_add_nvlist_array(m, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)f, 2);
		nvlist_t *r = fnvlist_alloc();
		fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
		fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
		    (const nvlist_t **)&m, 1);
		VERIFY0(spa_create("zt", r, NULL, NULL, NULL, NULL));
		VERIFY0(spa_export("zt", NULL, B_FALSE, B_FALSE));
		(void) printf("created\n");
	} else {
		importargs_t args = { 0 };
		char *paths[1] = { argv[1] };
		libpc_handle_t lpch = { .lpc_lib_handle = NULL,
		    .lpc_ops = &libzpool_config_ops, .lpc_printerr = B_TRUE };
		nvlist_t *cfg = NULL;
		args.path = paths;
		args.paths = 1;
		args.can_be_active = B_TRUE;
		VERIFY0(zpool_find_config(&lpch, "zt", &cfg, &args));
		VERIFY0(spa_import("zt", cfg, NULL, 0));
		VERIFY0(spa_open("zt", &spa, FTAG));
		vdev_t *vd = find_suffix(spa->spa_root_vdev, "/vdev1");
		VERIFY3P(vd, !=, NULL);
		int e = spa_vdev_detach(spa, vd->vdev_guid, 0, B_FALSE);
		(void) printf("detach: %d\n", e);
		spa_close(spa, FTAG);
		VERIFY0(spa_export("zt", NULL, B_FALSE, B_FALSE));
	}
	kernel_fini();
	return (0);
}
