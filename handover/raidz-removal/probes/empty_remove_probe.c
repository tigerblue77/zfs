/*
 * Scratch probe (not for upstream): userspace libzpool, sparse file vdevs only.
 * Drives spa_vdev_add()/spa_vdev_remove() directly to demonstrate the removal
 * of empty top-level vdevs (PR1) and to show that everything else keeps its
 * result.  Built twice, against the baseline tree and against the patched
 * tree.  The expected result of every check depends on the mode: "old" is the
 * unmodified tree, "new" is the patched one.
 *
 * Usage: empty_remove_probe <scratch-root> <old|new> [scenario ...]
 *
 * Every scenario runs in its own forked process (own kernel_init) in its own
 * sub-directory of the scratch root and, where it makes sense, leaves its pool
 * exported so that zdb can be run on it afterwards.  The names of those
 * pools are appended to <scratch-root>/manifest.txt.
 */
#include <libzpool.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_removal.h>
#include <sys/vdev_initialize.h>
#include <sys/vdev_trim.h>
#include <sys/metaslab_impl.h>
#include <sys/space_map.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/txg.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_scan.h>
#include <sys/zfs_debug.h>
#include <sys/fs/zfs.h>
#include <sys/zfeature.h>
#include <sys/zio_impl.h>
#include <sys/zio.h>
#include <sys/zfs_ioctl.h>
#include <libnvpair.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>

extern uint64_t raidz_expand_max_reflow_bytes;
extern uint_t raidz_expand_pause_point;

static int is_new;
static const char *root;
static char dir[1024];
static char poolname[64];
static int nfile;
static uint64_t fsize = 512ULL << 20;
static int npass, nfail;

#define	ZERR_CKPT	ZFS_ERR_CHECKPOINT_EXISTS
#define	ZERR_RZEXP	ZFS_ERR_RAIDZ_EXPAND_IN_PROGRESS

static void
chk(boolean_t ok, const char *fmt, ...)
{
	va_list ap;
	char buf[512];

	va_start(ap, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, ap);
	va_end(ap);
	(void) printf("  %s %s\n", ok ? "PASS" : "FAIL", buf);
	if (ok)
		npass++;
	else
		nfail++;
}

/* The result must be exp_new on the patched tree and exp_old otherwise. */
static void
chk_err(const char *what, int got, int exp_new, int exp_old)
{
	int exp = is_new ? exp_new : exp_old;

#ifdef HAVE_NOT_EMPTY_ERR
	/* A vdev that holds data in a pool that cannot be evacuated. */
	if (is_new && exp == EINVAL)
		exp = ZFS_ERR_VDEV_NOT_EMPTY;
#endif

	chk(got == exp, "%s: error %d (expected %d)", what, got, exp);
}

/*
 * Helpers to build configs.
 */
static nvlist_t *
mk_file(uint64_t ashift)
{
	char path[1100];
	int fd;

	(void) snprintf(path, sizeof (path), "%s/vd.%d", dir, nfile++);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	VERIFY3S(fd, >=, 0);
	VERIFY0(ftruncate(fd, fsize));
	(void) close(fd);

	nvlist_t *f = fnvlist_alloc();
	fnvlist_add_string(f, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
	fnvlist_add_string(f, ZPOOL_CONFIG_PATH, path);
	fnvlist_add_uint64(f, ZPOOL_CONFIG_ASHIFT, ashift);
	return (f);
}

static nvlist_t *
mk_top_ex(const char *type, int n, int nparity, int islog, const char *bias,
    uint64_t ashift)
{
	if (strcmp(type, VDEV_TYPE_FILE) == 0) {
		nvlist_t *f = mk_file(ashift);
		fnvlist_add_uint64(f, ZPOOL_CONFIG_IS_LOG, islog);
		if (bias != NULL)
			fnvlist_add_string(f, ZPOOL_CONFIG_ALLOCATION_BIAS,
			    bias);
		return (f);
	}
	nvlist_t **c = malloc(n * sizeof (nvlist_t *));
	nvlist_t *t = fnvlist_alloc();

	for (int i = 0; i < n; i++)
		c[i] = mk_file(ashift);
	fnvlist_add_string(t, ZPOOL_CONFIG_TYPE, type);
	if (strcmp(type, VDEV_TYPE_RAIDZ) == 0 ||
	    strcmp(type, VDEV_TYPE_DRAID) == 0)
		fnvlist_add_uint64(t, ZPOOL_CONFIG_NPARITY, nparity);
	if (strcmp(type, VDEV_TYPE_DRAID) == 0) {
		uint64_t ndata = 2, nspares = 1, ngroups = 1;

		while (ngroups * (ndata + nparity) % (n - nspares) != 0)
			ngroups++;
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NDATA, ndata);
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NSPARES, nspares);
		fnvlist_add_uint64(t, ZPOOL_CONFIG_DRAID_NGROUPS, ngroups);
	}
	fnvlist_add_uint64(t, ZPOOL_CONFIG_IS_LOG, islog);
	if (bias != NULL)
		fnvlist_add_string(t, ZPOOL_CONFIG_ALLOCATION_BIAS, bias);
	fnvlist_add_nvlist_array(t, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)c, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(c[i]);
	free(c);
	return (t);
}

static nvlist_t *
mk_top(const char *type, int n, int nparity)
{
	return (mk_top_ex(type, n, nparity, 0, NULL, 9));
}

static nvlist_t *
mk_root(nvlist_t **tops, int n)
{
	nvlist_t *r = fnvlist_alloc();

	fnvlist_add_string(r, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
	fnvlist_add_nvlist_array(r, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t **)tops, n);
	for (int i = 0; i < n; i++)
		fnvlist_free(tops[i]);
	return (r);
}

static void
mkpool_ex(nvlist_t *root_nv, const char *skip_feature)
{
	nvlist_t *props = fnvlist_alloc();

	for (int i = 0; i < SPA_FEATURES; i++) {
		char *buf;

		if (!spa_feature_table[i].fi_zfs_mod_supported)
			continue;
		if (skip_feature != NULL &&
		    strstr(skip_feature, spa_feature_table[i].fi_uname) != NULL)
			continue;
		VERIFY3S(-1, !=, asprintf(&buf, "feature@%s",
		    spa_feature_table[i].fi_uname));
		fnvlist_add_uint64(props, buf, 0);
		free(buf);
	}
	VERIFY0(spa_create(poolname, root_nv, props, NULL, NULL, NULL));
	fnvlist_free(props);
	fnvlist_free(root_nv);
}

static void
mkpool(nvlist_t *root_nv)
{
	mkpool_ex(root_nv, NULL);
}

/* A pool with a single top-level vdev. */
static void
mkpool_top(nvlist_t *top)
{
	nvlist_t *tops[1] = { top };

	mkpool(mk_root(tops, 1));
}

static int
add_top(nvlist_t *top)
{
	spa_t *spa;
	nvlist_t *tops[1] = { top };
	nvlist_t *rt = mk_root(tops, 1);

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int err = spa_vdev_add(spa, rt, B_FALSE);
	fnvlist_free(rt);
	spa_close(spa, FTAG);
	return (err);
}

static void
sync_n(int n)
{
	spa_t *spa;

	for (int i = 0; i < n; i++) {
		VERIFY0(spa_open(poolname, &spa, FTAG));
		txg_wait_synced(spa_get_dsl(spa), 0);
		spa_close(spa, FTAG);
	}
}

static void
set_comment(int i)
{
	spa_t *spa;
	char buf[64];
	nvlist_t *nvp = fnvlist_alloc();

	(void) snprintf(buf, sizeof (buf), "probe comment %d", i);
	fnvlist_add_string(nvp, "comment", buf);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	(void) spa_prop_set(spa, nvp);
	spa_close(spa, FTAG);
	fnvlist_free(nvp);
}

static void
churn(int rounds)
{
	for (int i = 1; i <= rounds; i++) {
		set_comment(i);
		sync_n(1);
	}
}

static uint64_t
top_guid(uint64_t id)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	uint64_t g = vdev_lookup_top(spa, id)->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static uint64_t
leaf_guid(uint64_t id, uint64_t leaf)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	uint64_t g = vdev_lookup_top(spa, id)->vdev_child[leaf]->vdev_guid;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (g);
}

static int
do_remove(uint64_t guid)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_remove(spa, guid, B_FALSE);
	spa_close(spa, FTAG);
	return (e);
}

static int
do_noalloc(uint64_t guid)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_noalloc(spa, guid);
	spa_close(spa, FTAG);
	return (e);
}

/*
 * Snapshots of the pool accounting.
 */
typedef struct snap {
	uint64_t nonalloc, root_space, root_dspace, root_alloc;
	uint64_t n_space, n_dspace, n_alloc, sp_space, dd_space;
	uint64_t spa_dspace, avz, children;
} snap_t;

static void
take_snap(snap_t *s)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	s->nonalloc = spa->spa_nonallocating_dspace;
	s->root_space = rvd->vdev_stat.vs_space;
	s->root_dspace = rvd->vdev_stat.vs_dspace;
	s->root_alloc = rvd->vdev_stat.vs_alloc;
	s->n_space = metaslab_class_get_space(spa_normal_class(spa));
	s->n_dspace = metaslab_class_get_dspace(spa_normal_class(spa));
	s->n_alloc = metaslab_class_get_alloc(spa_normal_class(spa));
	s->sp_space = metaslab_class_get_space(spa_special_class(spa));
	s->dd_space = metaslab_class_get_space(spa_dedup_class(spa));
	s->spa_dspace = spa_get_dspace(spa);
	s->children = rvd->vdev_children;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	uint64_t cnt = 0;
	(void) zap_count(spa->spa_meta_objset, spa->spa_all_vdev_zaps, &cnt);
	s->avz = cnt;
	spa_close(spa, FTAG);
}

static void
print_snap(const char *lbl, const snap_t *s)
{
	(void) printf("    snap %-22s nonalloc=%llu root_space=%llu "
	    "root_dspace=%llu normal_space=%llu normal_dspace=%llu "
	    "special_space=%llu dedup_space=%llu spa_dspace=%llu avz=%llu "
	    "children=%llu\n", lbl, (u_longlong_t)s->nonalloc,
	    (u_longlong_t)s->root_space, (u_longlong_t)s->root_dspace,
	    (u_longlong_t)s->n_space, (u_longlong_t)s->n_dspace,
	    (u_longlong_t)s->sp_space, (u_longlong_t)s->dd_space,
	    (u_longlong_t)s->spa_dspace, (u_longlong_t)s->avz,
	    (u_longlong_t)s->children);
}

/* The space accounting of the pool is the same as in the snapshot. */
static boolean_t
snap_same(const snap_t *a, const snap_t *b)
{
	return (a->nonalloc == b->nonalloc && a->root_space == b->root_space &&
	    a->root_dspace == b->root_dspace && a->n_space == b->n_space &&
	    a->n_dspace == b->n_dspace && a->sp_space == b->sp_space &&
	    a->dd_space == b->dd_space && a->spa_dspace == b->spa_dspace);
}

static void
tree(const char *lbl)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *rvd = spa->spa_root_vdev;
	(void) printf("    tree %s (children=%llu):\n", lbl,
	    (u_longlong_t)rvd->vdev_children);
	for (uint64_t c = 0; c < rvd->vdev_children; c++) {
		vdev_t *vd = rvd->vdev_child[c];
		(void) printf("      id %llu type=%-8s hole=%d log=%llu "
		    "alloc=%llu space=%llu noalloc=%llu removing=%llu "
		    "ms=%llu mg=%s\n", (u_longlong_t)vd->vdev_id,
		    vd->vdev_ops->vdev_op_type, (int)vd->vdev_ishole,
		    (u_longlong_t)vd->vdev_islog,
		    (u_longlong_t)vd->vdev_stat.vs_alloc,
		    (u_longlong_t)vd->vdev_stat.vs_space,
		    (u_longlong_t)vd->vdev_noalloc,
		    (u_longlong_t)vd->vdev_removing,
		    (u_longlong_t)vd->vdev_ms_count,
		    vd->vdev_mg != NULL ? "yes" : "no");
	}
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
}

static const char *
top_type(uint64_t id)
{
	spa_t *spa;
	const char *t;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	t = spa->spa_root_vdev->vdev_child[id]->vdev_ops->vdev_op_type;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (t);
}

static uint64_t
top_field(uint64_t id, const char *what)
{
	spa_t *spa;
	uint64_t v = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	vdev_t *vd = spa->spa_root_vdev->vdev_child[id];
	if (strcmp(what, "alloc") == 0)
		v = vd->vdev_stat.vs_alloc;
	else if (strcmp(what, "noalloc") == 0)
		v = vd->vdev_noalloc;
	else if (strcmp(what, "removing") == 0)
		v = vd->vdev_removing;
	else if (strcmp(what, "ms_array") == 0)
		v = vd->vdev_ms_array;
	else if (strcmp(what, "top_zap") == 0)
		v = vd->vdev_top_zap;
	else if (strcmp(what, "children") == 0)
		v = spa->spa_root_vdev->vdev_children;
	else if (strcmp(what, "ishole") == 0)
		v = vd->vdev_ishole;
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	return (v);
}

/*
 * MOS objects belonging to a vdev.
 */
typedef struct vobjs {
	uint64_t o[1024];
	int n;
} vobjs_t;

static void
vobj_add(vobjs_t *v, uint64_t o)
{
	if (o != 0 && v->n < 1024)
		v->o[v->n++] = o;
}

static void
collect_objs(spa_t *spa, vdev_t *vd, vobjs_t *v)
{
	for (int c = 0; c < vd->vdev_children; c++)
		collect_objs(spa, vd->vdev_child[c], v);
	if (vd->vdev_ops->vdev_op_leaf) {
		vobj_add(v, vd->vdev_leaf_zap);
		if (vd->vdev_dtl_sm != NULL)
			vobj_add(v, space_map_object(vd->vdev_dtl_sm));
	}
	if (vd == vd->vdev_top) {
		vobj_add(v, vd->vdev_top_zap);
		vobj_add(v, vd->vdev_ms_array);
		for (uint64_t m = 0; m < vd->vdev_ms_count; m++) {
			metaslab_t *msp = vd->vdev_ms[m];

			if (msp != NULL && msp->ms_sm != NULL)
				vobj_add(v, space_map_object(msp->ms_sm));
		}
		if (vd->vdev_top_zap != 0) {
			uint64_t obj = 0;

			if (zap_lookup(spa->spa_meta_objset, vd->vdev_top_zap,
			    VDEV_TOP_ZAP_MS_UNFLUSHED_PHYS_TXGS,
			    sizeof (uint64_t), 1, &obj) == 0)
				vobj_add(v, obj);
		}
	}
}

static void
get_objs(uint64_t id, vobjs_t *v)
{
	spa_t *spa;

	v->n = 0;
	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	collect_objs(spa, spa->spa_root_vdev->vdev_child[id], v);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
}

static int
objs_still_allocated(const vobjs_t *v)
{
	spa_t *spa;
	int alive = 0;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (int i = 0; i < v->n; i++) {
		dmu_object_info_t doi;

		if (dmu_object_info(spa->spa_meta_objset, v->o[i], &doi) == 0)
			alive++;
	}
	spa_close(spa, FTAG);
	return (alive);
}

static boolean_t
history_has(const char *needle)
{
	spa_t *spa;
	uint64_t off = 0, len;
	size_t cap = 1 << 20;
	char *buf = malloc(cap);
	boolean_t found = B_FALSE;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (;;) {
		len = cap;
		if (spa_history_get(spa, &off, &len, buf) != 0 || len == 0)
			break;
		if (memmem(buf, len, needle, strlen(needle)) != NULL)
			found = B_TRUE;
	}
	spa_close(spa, FTAG);
	free(buf);
	return (found);
}

/*
 * What a "zpool status" style consumer sees: the types of the top-level
 * entries of the pool config, and whether is_hole/REMOVING are present.
 */
static void
config_view(const char *lbl, int *nholes_out, int *nremoving_out)
{
	nvlist_t *cfg = NULL, *nvroot, **ch;
	uint_t nch;
	int nholes = 0, nrem = 0;

	VERIFY0(spa_get_stats(poolname, &cfg, NULL, 0));
	nvroot = fnvlist_lookup_nvlist(cfg, ZPOOL_CONFIG_VDEV_TREE);
	VERIFY0(nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    &ch, &nch));
	(void) printf("    config %s:", lbl);
	for (uint_t i = 0; i < nch; i++) {
		const char *t = fnvlist_lookup_string(ch[i],
		    ZPOOL_CONFIG_TYPE);
		uint64_t h = 0, r = 0;

		(void) nvlist_lookup_uint64(ch[i], ZPOOL_CONFIG_IS_HOLE, &h);
		(void) nvlist_lookup_uint64(ch[i], ZPOOL_CONFIG_REMOVING, &r);
		(void) printf(" [%s%s%s]", t, h ? " hole" : "",
		    r ? " removing" : "");
		nholes += (h != 0);
		nrem += (r != 0);
	}
	(void) printf("\n");
	if (nholes_out != NULL)
		*nholes_out = nholes;
	if (nremoving_out != NULL)
		*nremoving_out = nrem;
	fnvlist_free(cfg);
}

static boolean_t
scrub_clean(void)
{
	spa_t *spa;
	boolean_t ok;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	VERIFY0(spa_scan(spa, POOL_SCAN_SCRUB, 0));
	for (int i = 0; i < 600; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (!dsl_scan_scrubbing(spa_get_dsl(spa)))
			break;
		(void) usleep(100000);
	}
	dsl_scan_t *scn = spa_get_dsl(spa)->dp_scan;
	ok = !dsl_scan_scrubbing(spa_get_dsl(spa)) &&
	    scn->scn_phys.scn_state == DSS_FINISHED &&
	    scn->scn_phys.scn_errors == 0;
	(void) printf("    scrub: state=%llu errors=%llu examined=%llu\n",
	    (u_longlong_t)scn->scn_phys.scn_state,
	    (u_longlong_t)scn->scn_phys.scn_errors,
	    (u_longlong_t)scn->scn_phys.scn_examined);
	spa_close(spa, FTAG);
	return (ok);
}

/*
 * Export and import the pool again (the in-memory vdev tree is thrown away
 * and rebuilt from the MOS and the labels).
 */
static int
reimport(void)
{
	nvlist_t *cfg = NULL;

	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	int e = spa_import(poolname, cfg, NULL, 0);
	fnvlist_free(cfg);
	return (e);
}

/* Leave the pool exported for zdb and record it in the manifest. */
static void
leave_for_zdb(void)
{
	nvlist_t *cfg = NULL;
	char path[1200];
	FILE *f;

	VERIFY0(spa_export(poolname, &cfg, B_FALSE, B_FALSE));
	fnvlist_free(cfg);
	(void) snprintf(path, sizeof (path), "%s/manifest.txt", root);
	f = fopen(path, "a");
	if (f != NULL) {
		(void) fprintf(f, "%s %s\n", dir, poolname);
		(void) fclose(f);
	}
}

/*
 * Read the first vdev label of a file.  Returns the txg recorded in it, -1 if
 * there is no label at all.
 */
static int64_t
label_txg(const char *path)
{
	vdev_label_t *vl = malloc(sizeof (vdev_label_t));
	nvlist_t *nv = NULL;
	uint64_t txg = 0;
	int fd = open(path, O_RDONLY);
	int64_t r = -1;

	if (fd >= 0 && pread(fd, vl, sizeof (*vl), 0) == sizeof (*vl) &&
	    nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    sizeof (vl->vl_vdev_phys.vp_nvlist), &nv, 0) == 0) {
		if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_POOL_TXG, &txg) == 0)
			r = (int64_t)txg;
		nvlist_free(nv);
	}
	if (fd >= 0)
		(void) close(fd);
	free(vl);
	return (r);
}


/*
 * Look at the top-level vdev config (vdev_tree) in the first label of a leaf:
 * the removing and non-allocating flags and the metaslab array.
 */
static boolean_t
label_tree(const char *path, uint64_t *removing, uint64_t *noalloc,
    uint64_t *ms_array)
{
	vdev_label_t *vl = malloc(sizeof (vdev_label_t));
	nvlist_t *nv = NULL, *tree = NULL;
	int fd = open(path, O_RDONLY);
	boolean_t ok = B_FALSE;

	*removing = *noalloc = *ms_array = 0;
	if (fd >= 0 && pread(fd, vl, sizeof (*vl), 0) == sizeof (*vl) &&
	    nvlist_unpack(vl->vl_vdev_phys.vp_nvlist,
	    sizeof (vl->vl_vdev_phys.vp_nvlist), &nv, 0) == 0) {
		if (nvlist_lookup_nvlist(nv, ZPOOL_CONFIG_VDEV_TREE,
		    &tree) == 0) {
			(void) nvlist_lookup_uint64(tree,
			    ZPOOL_CONFIG_REMOVING, removing);
			(void) nvlist_lookup_uint64(tree,
			    ZPOOL_CONFIG_NONALLOCATING, noalloc);
			(void) nvlist_lookup_uint64(tree,
			    ZPOOL_CONFIG_METASLAB_ARRAY, ms_array);
			ok = B_TRUE;
		}
		nvlist_free(nv);
	}
	if (fd >= 0)
		(void) close(fd);
	free(vl);
	return (ok);
}

static boolean_t
feature_active(spa_feature_t f)
{
	spa_t *spa;
	boolean_t a;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	a = spa_feature_is_active(spa, f);
	spa_close(spa, FTAG);
	return (a);
}

static boolean_t
feature_enabled(spa_feature_t f)
{
	spa_t *spa;
	boolean_t e;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	e = spa_feature_is_enabled(spa, f);
	spa_close(spa, FTAG);
	return (e);
}

static void
wait_removal_done(void)
{
	spa_t *spa;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (int i = 0; i < 600; i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		if (spa->spa_removing_phys.sr_state != DSS_SCANNING &&
		    spa->spa_vdev_removal == NULL)
			break;
		(void) usleep(100000);
	}
	spa_close(spa, FTAG);
}

/*
 * Common scenario: a base pool and one more top-level vdev, which is empty.
 * Remove the added vdev.  The unmodified tree refuses with EINVAL.
 */
static void
add_remove_common(nvlist_t *base, nvlist_t *addvdev, uint64_t newid,
    boolean_t pre_noalloc)
{
	snap_t s0, s1, s2;
	vobjs_t vo;

	mkpool_top(base);
	sync_n(2);
	take_snap(&s0);
	chk(add_top(addvdev) == 0, "add of the new top-level vdev");
	if (pre_noalloc)
		chk(do_noalloc(top_guid(newid)) == 0,
		    "zpool set allocating=off on the new vdev");
	sync_n(1);
	take_snap(&s1);
	print_snap("before-add", &s0);
	print_snap("after-add", &s1);
	tree("after add");
	chk(top_field(newid, "alloc") == 0, "new vdev is empty (alloc=0)");
	get_objs(newid, &vo);
	(void) printf("    new vdev owns %d MOS objects\n", vo.n);

	int e = do_remove(top_guid(newid));
	chk_err("remove of the new empty vdev", e, 0, EINVAL);
	tree("after remove");
	take_snap(&s2);
	print_snap("after-remove", &s2);
	if (is_new) {
		int nh = 0, nr = 0;

		chk(top_field(newid, "ishole") == 1 &&
		    strcmp(top_type(newid), "hole") == 0,
		    "the slot of the new vdev is a hole");
		chk(snap_same(&s0, &s2),
		    "space accounting equals the one before the add");
		chk(s2.avz == s0.avz, "all vdev ZAPs freed (avz %llu == %llu)",
		    (u_longlong_t)s2.avz, (u_longlong_t)s0.avz);
		chk(objs_still_allocated(&vo) == 0,
		    "every MOS object of the vdev was freed (%d tracked)",
		    vo.n);
		chk(history_has("vdev remove"), "history has 'vdev remove'");
		config_view("after remove", &nh, &nr);
		chk(nh == 1 && nr == 0,
		    "config: one hole, no REMOVING flag");
		chk(!feature_active(SPA_FEATURE_DEVICE_REMOVAL),
		    "device_removal feature is not activated");
		chk(reimport() == 0, "export + import");
		tree("after export/import");
		chk(strcmp(top_type(newid), "hole") == 0,
		    "hole survived import");
		chk(scrub_clean(), "scrub clean");
		chk(add_top(mk_top("file", 1, 0)) == 0, "next add works");
		chk(top_field(0, "children") == newid + 2,
		    "next add got a new id (children=%llu, hole slot kept)",
		    (u_longlong_t)top_field(0, "children"));
	} else {
		chk(top_field(newid, "noalloc") == (pre_noalloc ? 1 : 0),
		    "refusal left the allocation state of the vdev alone");
		chk(top_field(newid, "removing") == 0,
		    "refusal left removing clear");
		chk(snap_same(&s1, &s2),
		    "refusal left the space accounting alone");
	}
	leave_for_zdb();
}

static void
sc_raidz_add_raidz(void)
{
	add_remove_common(mk_top("raidz", 3, 1), mk_top("raidz", 3, 2), 1,
	    B_FALSE);
}

static void
sc_raidz_add_raidz_noalloc(void)
{
	add_remove_common(mk_top("raidz", 3, 1), mk_top("raidz", 3, 1), 1,
	    B_TRUE);
}

static void
sc_raidz_add_disk(void)
{
	add_remove_common(mk_top("raidz", 3, 1), mk_top("file", 1, 0), 1,
	    B_FALSE);
}

static void
sc_raidz_add_mirror(void)
{
	add_remove_common(mk_top("raidz", 3, 1), mk_top("mirror", 2, 0), 1,
	    B_FALSE);
}

static void
sc_raidz_add_special(void)
{
	add_remove_common(mk_top("raidz", 3, 1),
	    mk_top_ex("mirror", 2, 0, 0, VDEV_ALLOC_BIAS_SPECIAL, 9), 1,
	    B_FALSE);
}

static void
sc_raidz_add_dedup(void)
{
	add_remove_common(mk_top("raidz", 3, 1),
	    mk_top_ex("file", 1, 0, 0, VDEV_ALLOC_BIAS_DEDUP, 9), 1, B_FALSE);
}

static void
sc_mirror_add_raidz(void)
{
	add_remove_common(mk_top("mirror", 2, 0), mk_top("raidz", 3, 1), 1,
	    B_FALSE);
}

static void
sc_draid_add_disk(void)
{
	add_remove_common(mk_top("draid", 5, 1), mk_top("file", 1, 0), 1,
	    B_FALSE);
}

/*
 * dRAID targets are never removed this way.
 */
static int
spare_count(void)
{
	spa_t *spa;
	int n;

	VERIFY0(spa_open(poolname, &spa, FTAG));
	n = spa->spa_spares.sav_count;
	spa_close(spa, FTAG);
	return (n);
}

static void
sc_draid_add_draid(void)
{
	mkpool_top(mk_top("draid", 5, 1));
	sync_n(2);
	chk(add_top(mk_top("draid", 5, 1)) == 0, "add of a second dRAID");
	sync_n(1);
	tree("after add");
	chk(spare_count() == 2, "two distributed spares (%d)", spare_count());
	chk(top_field(1, "alloc") == 0, "new dRAID is empty");
	chk_err("remove of the empty dRAID", do_remove(top_guid(1)),
	    0, EINVAL);
	tree("after remove");
	if (is_new) {
		chk(strcmp(top_type(1), "hole") == 0, "dRAID became a hole");
		chk(spare_count() == 1, "its distributed spare is gone (%d)",
		    spare_count());
		chk(feature_active(SPA_FEATURE_DRAID),
		    "draid stays active for the first dRAID");
		/* libzpool's spa_import() drops draid spares: see ZTS */
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}

static void
sc_mirror_add_draid(void)
{
	mkpool_top(mk_top("mirror", 2, 0));
	sync_n(2);
	chk(!feature_active(SPA_FEATURE_DRAID), "draid not active at first");
	chk(add_top(mk_top("draid", 5, 1)) == 0, "add of a dRAID");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(2);
	chk(feature_active(SPA_FEATURE_DRAID), "draid active after the add");
	chk(spare_count() == 1, "one distributed spare");
	chk_err("remove of the empty dRAID", do_remove(top_guid(1)),
	    0, 0);
	tree("after remove");
	if (is_new) {
		sync_n(1);
		chk(strcmp(top_type(1), "hole") == 0, "dRAID became a hole");
		chk(spare_count() == 0, "no spare left");
		chk(!feature_active(SPA_FEATURE_DRAID),
		    "draid is no longer active");
		chk(!feature_active(SPA_FEATURE_DEVICE_REMOVAL),
		    "device_removal not activated");
		chk(reimport() == 0, "export + import");
		chk(!feature_active(SPA_FEATURE_DRAID),
		    "draid still inactive after import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}
/*
 * Behavior of what worked before: the empty disk is evacuated and becomes an
 * indirect vdev, with or without the patch.
 */
static void
sc_noraidz_evacuation(void)
{
	mkpool_top(mk_top("mirror", 2, 0));
	sync_n(2);
	chk(add_top(mk_top("file", 1, 0)) == 0, "add of a disk");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(1);
	tree("after add");
	chk_err("remove of the empty disk in a pool without raidz",
	    do_remove(top_guid(1)), 0, 0);
	if (is_new) {
		tree("after removal");
		chk(strcmp(top_type(1), "hole") == 0, "vdev became a hole");
		chk(!feature_active(SPA_FEATURE_DEVICE_REMOVAL),
		    "device_removal feature is not active");
		chk(history_has("vdev remove") &&
		    !history_has("vdev remove started"),
		    "history has the hole record only");
	} else {
		wait_removal_done();
		chk(strcmp(top_type(1), "indirect") == 0,
		    "vdev became indirect");
	}
	chk(reimport() == 0, "export + import");
	chk(scrub_clean(), "scrub clean");

	/* A vdev that holds data is still evacuated. */
	chk(add_top(mk_top("file", 1, 0)) == 0, "add of another disk");
	sync_n(1);
	churn(4);
	uint64_t id = top_field(0, "children") - 1;
	chk(top_field(id, "alloc") != 0, "it holds data");
	chk_err("remove of the disk with data", do_remove(top_guid(id)), 0, 0);
	wait_removal_done();
	chk(strcmp(top_type(id), "indirect") == 0,
	    "the disk with data became indirect");
	chk(feature_active(SPA_FEATURE_DEVICE_REMOVAL),
	    "device_removal feature is active");
	chk(reimport() == 0, "export + import");
	chk(scrub_clean(), "scrub clean");
	leave_for_zdb();
}
/*
 * Pools that could not remove the vdev before and still cannot: a mixed ashift
 * without any raidz.
 */
static void
sc_mixed_noraidz(void)
{
	mkpool_top(mk_top("file", 1, 0));
	sync_n(2);
	chk(add_top(mk_top_ex("file", 1, 0, 0, NULL, 12)) == 0,
	    "add of an ashift 12 disk");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(1);
	tree("after add");
	chk(top_field(1, "alloc") == 0, "new vdev is empty");
	chk_err("remove of the empty ashift 12 disk without raidz",
	    do_remove(top_guid(1)), 0, EINVAL);
	tree("after remove");
	if (is_new) {
		chk(strcmp(top_type(1), "hole") == 0, "vdev became a hole");
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}
static void
sc_mixed_raidz(void)
{
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top_ex("raidz", 3, 1, 0, NULL, 12)) == 0,
	    "add of an ashift 12 raidz");
	sync_n(1);
	tree("after add");
	chk(top_field(1, "alloc") == 0, "new vdev is empty");
	chk_err("remove of the empty ashift 12 raidz",
	    do_remove(top_guid(1)), 0, EINVAL);
	tree("after remove");
	if (is_new) {
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}

/*
 * remove_raidz.ksh layout: neither vdev is empty, nothing changes.
 */
static void
sc_create2(void)
{
	snap_t s1, s2;
	nvlist_t *t[2] = { mk_top("file", 1, 0), mk_top("raidz", 2, 1) };

	mkpool(mk_root(t, 2));
	sync_n(2);
	take_snap(&s1);
	tree("after create");
	chk(top_field(0, "alloc") > 0 && top_field(1, "alloc") > 0,
	    "both vdevs hold the pool metadata");
	chk_err("remove of the disk top-level", do_remove(top_guid(0)),
	    EINVAL, EINVAL);
	chk_err("remove of a raidz leaf", do_remove(leaf_guid(1, 0)),
	    ENOTSUP, ENOTSUP);
	chk_err("remove of the raidz top-level", do_remove(top_guid(1)),
	    EINVAL, EINVAL);
	take_snap(&s2);
	tree("after the three attempts");
	chk(top_field(0, "noalloc") == 0 && top_field(1, "noalloc") == 0 &&
	    top_field(0, "removing") == 0 && top_field(1, "removing") == 0,
	    "no vdev was left passivated or removing");
	chk(snap_same(&s1, &s2), "accounting unchanged");
	leave_for_zdb();
}

/*
 * A vdev that holds data is refused exactly as before (the upfront alloc test
 * leaves it untouched).
 */
static void
sc_populated(void)
{
	snap_t s1, s2;
	int rounds = 0;

	fsize = 40ULL << 30;
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	while (top_field(1, "alloc") == 0 && rounds < 40) {
		churn(1);
		rounds++;
	}
	tree("after churn");
	(void) printf("    rounds of churn: %d\n", rounds);
	if (top_field(1, "alloc") == 0) {
		(void) printf("  SKIP the new vdev stayed empty\n");
		return;
	}
	take_snap(&s1);
	chk_err("remove of the vdev that holds data", do_remove(top_guid(1)),
	    EINVAL, EINVAL);
	take_snap(&s2);
	tree("after refused removal");
	chk(top_field(1, "noalloc") == 0 && top_field(1, "removing") == 0,
	    "refusal left the vdev allocating");
	chk(snap_same(&s1, &s2), "refusal left the accounting alone");

	/* A user set non-allocating state survives the refusal. */
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	take_snap(&s1);
	chk(top_field(1, "alloc") > 0, "vdev still holds data");
	chk_err("remove of the non-allocating vdev that holds data",
	    do_remove(top_guid(1)), EINVAL, EINVAL);
	take_snap(&s2);
	chk(top_field(1, "noalloc") == 1, "vdev is still non-allocating");
	chk(snap_same(&s1, &s2), "refusal left the accounting alone");
	chk(reimport() == 0, "export + import");
	chk(scrub_clean(), "scrub clean");
	leave_for_zdb();
	fsize = 512ULL << 20;
}

/*
 * The recommended workflow: set allocating=off right after the add, keep the
 * pool busy, remove.  Hazard 1 (spa_nonallocating_dspace) is checked here.
 */
static void
sc_churn_noalloc(void)
{
	snap_t s0, s1, s2;

	fsize = 40ULL << 30;
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	take_snap(&s0);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	take_snap(&s1);
	print_snap("noalloc, no churn", &s1);
	churn(8);
	tree("after 8 rounds of churn");
	chk(top_field(1, "alloc") == 0, "new vdev is still empty");
	chk(top_field(0, "alloc") > 0, "the metadata went to the base vdev");
	chk_err("remove of the empty non-allocating vdev",
	    do_remove(top_guid(1)), 0, EINVAL);
	take_snap(&s2);
	print_snap("after remove", &s2);
	if (is_new) {
		chk(s2.nonalloc == s0.nonalloc && s2.nonalloc == 0,
		    "spa_nonallocating_dspace back to %llu",
		    (u_longlong_t)s0.nonalloc);
		chk(s2.root_space == s0.root_space &&
		    s2.n_space == s0.n_space && s2.n_dspace == s0.n_dspace &&
		    s2.spa_dspace == s0.spa_dspace,
		    "pool size back to the one before the add");
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	} else {
		chk(top_field(1, "noalloc") == 1,
		    "refusal kept the non-allocating state");
	}
	leave_for_zdb();
	fsize = 512ULL << 20;
}

static void
sc_checkpoint(void)
{
	spa_t *spa;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	chk(top_field(1, "alloc") == 0, "new vdev is empty");
	chk(spa_checkpoint(poolname) == 0, "checkpoint created");
	chk_err("remove with a checkpoint", do_remove(top_guid(1)),
	    ZERR_CKPT, ZERR_CKPT);
	tree("after refused removal");
	chk(top_field(1, "noalloc") == 1 && top_field(1, "removing") == 0,
	    "refusal changed nothing");
	chk(spa_checkpoint_discard(poolname) == 0, "checkpoint discard started");
	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (int i = 0; i < 600 &&
	    spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT); i++) {
		txg_wait_synced(spa_get_dsl(spa), 0);
		(void) usleep(100000);
	}
	chk(!spa_feature_is_active(spa, SPA_FEATURE_POOL_CHECKPOINT),
	    "checkpoint discarded");
	spa_close(spa, FTAG);
	chk(top_field(1, "alloc") == 0, "new vdev is still empty");
	chk_err("remove after the checkpoint is gone", do_remove(top_guid(1)),
	    0, EINVAL);
	tree("final");
	if (is_new) {
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}

/*
 * RAID-Z expansion of another vdev is running.
 */
static void
sc_expand(void)
{
	spa_t *spa;
	nvlist_t *tops[1];
	nvlist_t *rt;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	chk(top_field(1, "alloc") == 0, "new vdev is empty");

	raidz_expand_pause_point = 1;	/* park the expansion thread early */
	tops[0] = mk_file(9);
	rt = mk_root(tops, 1);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_attach(spa, top_guid(0), rt, B_FALSE, B_FALSE);
	fnvlist_free(rt);
	chk(e == 0, "raidz expansion of the base vdev started (%d)", e);
	for (int i = 0; i < 100 && spa->spa_raidz_expand == NULL; i++)
		(void) usleep(50000);
	chk(spa->spa_raidz_expand != NULL, "expansion is running");
	spa_close(spa, FTAG);

	chk_err("remove of the empty vdev during the expansion",
	    do_remove(top_guid(1)), ZERR_RZEXP, EINVAL);
	tree("during expansion");
	chk(top_field(1, "noalloc") == 1 && top_field(1, "removing") == 0,
	    "refusal changed nothing");

	raidz_expand_pause_point = 0;
	VERIFY0(spa_open(poolname, &spa, FTAG));
	for (int i = 0; i < 1200 && spa->spa_raidz_expand != NULL; i++)
		(void) usleep(100000);
	chk(spa->spa_raidz_expand == NULL, "expansion finished");
	spa_close(spa, FTAG);
	sync_n(2);
	chk_err("remove after the expansion", do_remove(top_guid(1)), 0,
	    EINVAL);
	tree("final");
	if (is_new) {
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}

static void
sc_feature_disabled(void)
{
	/* obsolete_counts depends on device_removal: skip both */
	{
		nvlist_t *tops[1] = { mk_top("raidz", 3, 1) };

		mkpool_ex(mk_root(tops, 1),
		    "device_removal obsolete_counts");
	}
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	chk_err("remove of the empty vdev with device_removal disabled",
	    do_remove(top_guid(1)), 0, ENOTSUP);
	if (is_new) {
		chk(strcmp(top_type(1), "hole") == 0, "vdev became a hole");
		chk(!feature_enabled(SPA_FEATURE_DEVICE_REMOVAL),
		    "device_removal stays disabled");
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	chk_err("remove of the vdev with data", do_remove(top_guid(0)),
	    ENOTSUP, ENOTSUP);
	leave_for_zdb();
}

static void
sc_expanded_empty(void)
{
	spa_t *spa;
	nvlist_t *tops[1];
	nvlist_t *rt;

	mkpool_top(mk_top("mirror", 2, 0));
	sync_n(2);
	/*
	 * A dedup-class vdev stays empty without allocating=off, which an
	 * expansion does not handle (spa_nonallocating_dspace).
	 */
	chk(add_top(mk_top_ex("raidz", 3, 1, 0, VDEV_ALLOC_BIAS_DEDUP,
	    9)) == 0, "add of a dedup raidz1");
	sync_n(1);
	tops[0] = mk_file(9);
	rt = mk_root(tops, 1);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_attach(spa, top_guid(1), rt, B_FALSE, B_FALSE);
	fnvlist_free(rt);
	chk(e == 0, "expansion of the empty raidz started (%d)", e);
	for (int i = 0; i < 1200 && (spa->spa_raidz_expand != NULL ||
	    !spa_feature_is_active(spa, SPA_FEATURE_RAIDZ_EXPANSION)); i++)
		(void) usleep(100000);
	chk(spa->spa_raidz_expand == NULL, "expansion finished");
	spa_close(spa, FTAG);
	sync_n(2);
	chk(feature_active(SPA_FEATURE_RAIDZ_EXPANSION),
	    "raidz_expansion is active");
	chk(top_field(1, "alloc") == 0, "the expanded raidz is empty");
	chk_err("remove of the expanded empty raidz", do_remove(top_guid(1)),
	    0, 0);
	tree("after remove");
	if (is_new) {
		sync_n(1);
		chk(strcmp(top_type(1), "hole") == 0, "raidz became a hole");
		chk(!feature_active(SPA_FEATURE_RAIDZ_EXPANSION),
		    "raidz_expansion is no longer active");
		chk(reimport() == 0, "export + import");
		chk(!feature_active(SPA_FEATURE_RAIDZ_EXPANSION),
		    "still inactive after import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}
/*
 * Log removal is the same as before.
 */
static void
sc_log(void)
{
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top_ex("file", 1, 0, 1, NULL, 9)) == 0, "add of a log");
	sync_n(1);
	tree("after add");
	chk_err("remove of the log", do_remove(top_guid(1)), 0, 0);
	tree("after remove");
	chk(strcmp(top_type(1), "hole") == 0, "log slot is a hole");
	chk(history_has("vdev remove"), "history has 'vdev remove'");
	chk(reimport() == 0, "export + import");
	chk(scrub_clean(), "scrub clean");
	leave_for_zdb();
}

/*
 * Repeated add/remove cycles with all types, half of them after noalloc.
 */
static void
sc_cycles(void)
{
	snap_t s0, s2;
	int n = 6;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	take_snap(&s0);
	for (int i = 0; i < n; i++) {
		nvlist_t *v;
		uint64_t id = top_field(0, "children");
		char what[96];
		const char *nm;

		switch (i % 6) {
		case 0:
			v = mk_top("raidz", 3, 2);
			nm = "raidz2";
			break;
		case 1:
			v = mk_top("file", 1, 0);
			nm = "disk";
			break;
		case 2:
			v = mk_top("mirror", 2, 0);
			nm = "mirror";
			break;
		case 3:
			v = mk_top("raidz", 3, 1);
			nm = "raidz1";
			break;
		case 4:
			v = mk_top_ex("mirror", 2, 0, 0, VDEV_ALLOC_BIAS_SPECIAL,
			    9);
			nm = "special mirror";
			break;
		default:
			v = mk_top_ex("file", 1, 0, 0, VDEV_ALLOC_BIAS_DEDUP,
			    9);
			nm = "dedup disk";
			break;
		}
		chk(add_top(v) == 0, "cycle %d: add %s as id %llu", i, nm,
		    (u_longlong_t)id);
		if (i % 2 == 1)
			chk(do_noalloc(top_guid(id)) == 0,
			    "cycle %d: allocating=off", i);
		(void) snprintf(what, sizeof (what), "cycle %d: remove %s", i,
		    nm);
		chk_err(what, do_remove(top_guid(id)), 0, EINVAL);
	}
	take_snap(&s2);
	tree("after the cycles");
	print_snap("before", &s0);
	print_snap("after", &s2);
	if (is_new) {
		int nh = 0, nr = 0;

		chk(snap_same(&s0, &s2), "accounting back to the start");
		chk(s2.avz == s0.avz, "no vdev ZAP leaked (avz %llu == %llu)",
		    (u_longlong_t)s2.avz, (u_longlong_t)s0.avz);
		config_view("after the cycles", &nh, &nr);
		chk(nh == n && nr == 0, "config: %d holes, no REMOVING", n);
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
		config_view("after import", &nh, &nr);
		chk(nh == n, "holes survived the import");
	}
	leave_for_zdb();
}

/*
 * A leaf of the target is offline: the other labels are still wiped.
 */
static void
sc_degraded(void)
{
	spa_t *spa;
	char paths[3][1200];
	int64_t t_before[3], t_after[3];
	int e;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(2);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	for (int i = 0; i < 3; i++)
		(void) strlcpy(paths[i],
		    spa->spa_root_vdev->vdev_child[1]->vdev_child[i]->vdev_path,
		    sizeof (paths[i]));
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	for (int i = 0; i < 3; i++)
		t_before[i] = label_txg(paths[i]);
	(void) printf("    label txgs before: %lld %lld %lld\n",
	    (long long)t_before[0], (long long)t_before[1],
	    (long long)t_before[2]);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	e = vdev_offline(spa, leaf_guid(1, 2), 0);
	spa_close(spa, FTAG);
	chk(e == 0, "leaf 2 of the new raidz taken offline (%d)", e);
	tree("with an offline leaf");
	e = do_remove(top_guid(1));
	(void) printf("    OBSERVE remove of a degraded empty raidz: error %d\n",
	    e);
	for (int i = 0; i < 3; i++)
		t_after[i] = label_txg(paths[i]);
	(void) printf("    label txgs after:  %lld %lld %lld\n",
	    (long long)t_after[0], (long long)t_after[1],
	    (long long)t_after[2]);
	if (is_new && e == 0) {
		chk(t_after[0] == 0 && t_after[1] == 0,
		    "live leaves got an uninitialized (txg 0) label");
		chk(t_after[2] == t_before[2],
		    "offline leaf keeps its old label (stale, txg %lld)",
		    (long long)t_after[2]);
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	} else if (!is_new) {
		chk(e == EINVAL || e == EBUSY, "old tree refuses (%d)", e);
	}
	leave_for_zdb();
}

/*
 * A scrub is running while the empty vdev is removed.
 */
static void
sc_scrub_running(void)
{
	spa_t *spa;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	VERIFY0(spa_open(poolname, &spa, FTAG));
	chk(spa_scan(spa, POOL_SCAN_SCRUB, 0) == 0, "scrub started");
	spa_close(spa, FTAG);
	chk_err("remove while scrubbing", do_remove(top_guid(1)), 0, EINVAL);
	if (is_new) {
		chk(scrub_clean(), "scrub finished clean");
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "second scrub clean");
	}
	leave_for_zdb();
}

/*
 * A running initialize on a leaf of the empty vdev.
 */
static void
sc_initialize(void)
{
	spa_t *spa;
	nvlist_t *nv = fnvlist_alloc(), *errl = fnvlist_alloc();
	vdev_t *leaf;
	int e;

	fsize = 2ULL << 30;
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(1);
	fnvlist_add_uint64(nv, "leaf0", leaf_guid(1, 0));
	VERIFY0(spa_open(poolname, &spa, FTAG));
	e = spa_vdev_initialize(spa, nv, POOL_INITIALIZE_START, 0, B_FALSE,
	    errl);
	chk(e == 0, "initialize started on a leaf (%d)", e);
	spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	leaf = spa->spa_root_vdev->vdev_child[1]->vdev_child[0];
	for (int i = 0; i < 200 && leaf->vdev_initialize_thread == NULL; i++) {
		spa_config_exit(spa, SCL_VDEV, FTAG);
		(void) usleep(10000);
		spa_config_enter(spa, SCL_VDEV, FTAG, RW_READER);
	}
	(void) printf("    initialize thread running: %s, state %d\n",
	    leaf->vdev_initialize_thread != NULL ? "yes" : "no",
	    (int)leaf->vdev_initialize_state);
	spa_config_exit(spa, SCL_VDEV, FTAG);
	spa_close(spa, FTAG);
	chk_err("remove while initializing", do_remove(top_guid(1)), 0,
	    EINVAL);
	tree("after remove");
	if (!is_new) {
		/* let the old tree finish quickly */
		VERIFY0(spa_open(poolname, &spa, FTAG));
		fnvlist_free(nv);
		nv = fnvlist_alloc();
		fnvlist_add_uint64(nv, "leaf0", leaf_guid(1, 0));
		(void) spa_vdev_initialize(spa, nv, POOL_INITIALIZE_CANCEL, 0,
		    B_FALSE, errl);
		spa_close(spa, FTAG);
	} else {
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
	fsize = 512ULL << 20;
}

static void
sc_trim(void)
{
	spa_t *spa;
	nvlist_t *nv = fnvlist_alloc(), *errl = fnvlist_alloc();
	int e;

	fsize = 1ULL << 30;
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	sync_n(1);
	fnvlist_add_uint64(nv, "leaf0", leaf_guid(1, 0));
	VERIFY0(spa_open(poolname, &spa, FTAG));
	e = spa_vdev_trim(spa, nv, POOL_TRIM_START, 0, B_FALSE, B_FALSE, errl);
	(void) printf("    OBSERVE trim start on a file vdev: error %d\n", e);
	spa_close(spa, FTAG);
	chk_err("remove while trimming", do_remove(top_guid(1)), 0, EINVAL);
	tree("after remove");
	if (is_new) {
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
	fsize = 512ULL << 20;
}

/*
 * What the kernel does between "zpool add" and "zpool remove": the history
 * record of the add is written afterwards.  Show whether it lands on the new
 * vdev.
 */
static void
sc_history(void)
{
	spa_t *spa;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	VERIFY0(spa_open(poolname, &spa, FTAG));
	chk(spa_history_log(spa, "zpool add probe raidz2") == 0,
	    "history record of the add written (not synced yet)");
	spa_close(spa, FTAG);
	(void) printf("    new vdev alloc before any sync: %llu\n",
	    (u_longlong_t)top_field(1, "alloc"));
	chk_err("remove right after the add and its history record",
	    do_remove(top_guid(1)), 0, EINVAL);
	tree("A: after remove");
	if (is_new)
		leave_for_zdb();
	else
		VERIFY0(spa_destroy(poolname));
}

static void
sc_history_sync(void)
{
	spa_t *spa;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	chk(add_top(mk_top("raidz", 3, 2)) == 0, "add of a raidz2");
	VERIFY0(spa_open(poolname, &spa, FTAG));
	chk(spa_history_log(spa, "zpool add probe raidz2") == 0,
	    "history record of the add written");
	spa_close(spa, FTAG);
	sync_n(1);
	(void) printf("    OBSERVE new vdev alloc after the sync that "
	    "wrote the history: %llu\n", (u_longlong_t)top_field(1, "alloc"));
	int e = do_remove(top_guid(1));
	(void) printf("    OBSERVE remove after a sync: error %d\n", e);
	tree("B: after remove");
	if (e == 0)
		leave_for_zdb();
	else
		VERIFY0(spa_destroy(poolname));
}



/*
 * The target is an empty raidz which was itself expanded earlier.
 */
static void
sc_expand_target(void)
{
	spa_t *spa;
	nvlist_t *tops[1];
	nvlist_t *rt;
	vobjs_t vo;
	snap_t s0, s2;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	take_snap(&s0);
	chk(add_top(mk_top("raidz", 3, 1)) == 0, "add of a raidz1");
	chk(do_noalloc(top_guid(1)) == 0, "zpool set allocating=off");
	tops[0] = mk_file(9);
	rt = mk_root(tops, 1);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	int e = spa_vdev_attach(spa, top_guid(1), rt, B_FALSE, B_FALSE);
	fnvlist_free(rt);
	chk(e == 0, "raidz expansion of the empty vdev started (%d)", e);
	for (int i = 0; i < 1200 && spa->spa_raidz_expand != NULL; i++)
		(void) usleep(100000);
	chk(spa->spa_raidz_expand == NULL, "expansion finished");
	spa_close(spa, FTAG);
	sync_n(2);
	tree("after expansion of the empty vdev");
	chk(top_field(1, "alloc") == 0, "expanded vdev is still empty");
	get_objs(1, &vo);
	(void) printf("    expanded vdev owns %d MOS objects\n", vo.n);
	chk_err("remove of the expanded empty raidz", do_remove(top_guid(1)),
	    0, EINVAL);
	take_snap(&s2);
	tree("after remove");
	if (is_new) {
		chk(strcmp(top_type(1), "hole") == 0, "vdev slot is a hole");
		chk(objs_still_allocated(&vo) == 0,
		    "every MOS object of the vdev was freed (%d tracked)",
		    vo.n);
		chk(s2.avz == s0.avz, "no vdev ZAP leaked (avz %llu == %llu)",
		    (u_longlong_t)s2.avz, (u_longlong_t)s0.avz);
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean");
	}
	leave_for_zdb();
}

/*
 * Concurrent metadata writers and scrubs while empty vdevs are added and
 * removed, with and without allocating=off.  A removal either succeeds, or the
 * vdev is not empty and the result is the old EINVAL (the undo paths).
 */
static volatile int stress_stop;
static volatile int stress_writes;

static void *
stress_writer(void *arg)
{
	(void) arg;

	for (int i = 0; !stress_stop; i++) {
		set_comment(i);
		stress_writes++;
		if (i % 4 == 0)
			sync_n(1);
	}
	return (NULL);
}

static void *
stress_scrubber(void *arg)
{
	spa_t *spa;

	(void) arg;
	while (!stress_stop) {
		VERIFY0(spa_open(poolname, &spa, FTAG));
		(void) spa_scan(spa, POOL_SCAN_SCRUB, 0);
		for (int i = 0; i < 200 && !stress_stop &&
		    dsl_scan_scrubbing(spa_get_dsl(spa)); i++)
			(void) usleep(50000);
		spa_close(spa, FTAG);
		(void) usleep(100000);
	}
	return (NULL);
}

static void
sc_stress(void)
{
	pthread_t w, sc;
	int nok = 0, nbusy = 0, nother = 0;
	snap_t s0, s1;
	int ncycles = 40;

	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	take_snap(&s0);
	VERIFY0(pthread_create(&w, NULL, stress_writer, NULL));
	VERIFY0(pthread_create(&sc, NULL, stress_scrubber, NULL));
	for (int i = 0; i < ncycles; i++) {
		uint64_t id = top_field(0, "children");
		nvlist_t *v;
		int e;

		switch (i % 4) {
		case 0:
			v = mk_top("raidz", 3, 1);
			break;
		case 1:
			v = mk_top("file", 1, 0);
			break;
		case 2:
			v = mk_top("mirror", 2, 0);
			break;
		default:
			v = mk_top_ex("mirror", 2, 0, 0,
			    VDEV_ALLOC_BIAS_SPECIAL, 9);
			break;
		}
		VERIFY0(add_top(v));
		if (i % 3 != 0)
			(void) do_noalloc(top_guid(id));
		e = do_remove(top_guid(id));
		if (e == 0)
			nok++;
#ifdef HAVE_NOT_EMPTY_ERR
		else if (e == ZFS_ERR_VDEV_NOT_EMPTY)
#else
		else if (e == EINVAL)
#endif
			nbusy++;
		else {
			nother++;
			(void) printf("    cycle %d: unexpected error %d\n", i,
			    e);
		}
	}
	stress_stop = 1;
	VERIFY0(pthread_join(w, NULL));
	VERIFY0(pthread_join(sc, NULL));
	(void) printf("    cycles=%d removed=%d refused(not empty)=%d "
	    "other=%d writes=%d\n", ncycles, nok, nbusy, nother,
	    stress_writes);
	chk(nother == 0, "every removal was either done or refused with EINVAL");
	if (is_new)
		chk(nok > 0, "some removals succeeded (%d)", nok);
	take_snap(&s1);
	print_snap("before", &s0);
	print_snap("after", &s1);
	tree("after the stress");
	chk(s1.nonalloc == 0 || nbusy > 0,
	    "no leaked non-allocating space (%llu)",
	    (u_longlong_t)s1.nonalloc);
	chk(reimport() == 0, "export + import");
	chk(scrub_clean(), "scrub clean");
	leave_for_zdb();
}

#ifdef HAVE_REMOVING_EMPTY
/*
 * Crash tests.  The grandchild builds the pool and dies inside the removal;
 * this process then imports what is on disk and retries.
 */
static volatile uint64_t watch_vdev_id;
static volatile int watch_arm;

static void *
watcher(void *arg)
{
	spa_t *spa = arg;

	while (!watch_arm)
		(void) usleep(100);
	for (;;) {
		vdev_t *rvd = spa->spa_root_vdev;
		vdev_t *vd = rvd->vdev_children > watch_vdev_id ?
		    rvd->vdev_child[watch_vdev_id] : NULL;

		if (vd != NULL && vd->vdev_removing_empty)
			abort();
	}
	return (NULL);
}

static void
crash_child(const char *how)
{
	spa_t *spa;
	char cfgpath[1200];
	nvlist_t *cfg;
	char *packed;
	size_t sz = 0;
	FILE *f;

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	mkpool_top(mk_top("raidz", 3, 1));
	sync_n(2);
	VERIFY0(add_top(mk_top("raidz", 3, 2)));
	if (strcmp(how, "after_x") == 0 || strcmp(how, "noalloc") == 0)
		VERIFY0(do_noalloc(top_guid(1)));
	sync_n(1);
	VERIFY0(spa_open(poolname, &spa, FTAG));
	cfg = spa_config_generate(spa, NULL, -1ULL, B_TRUE);
	packed = fnvlist_pack(cfg, &sz);
	(void) snprintf(cfgpath, sizeof (cfgpath), "%s/config.nv", dir);
	f = fopen(cfgpath, "w");
	VERIFY3P(f, !=, NULL);
	VERIFY3U(fwrite(packed, 1, sz, f), ==, sz);
	(void) fclose(f);
	fnvlist_pack_free(packed, sz);
	fnvlist_free(cfg);
	(void) printf("    child: pool ready, removing id 1 with crash '%s'\n",
	    how);
	(void) fflush(stdout);
	if (strcmp(how, "passivate") == 0) {
		zinject_record_t rec = { 0 };
		int id;

		rec.zi_cmd = ZINJECT_PANIC;
		(void) strlcpy(rec.zi_func, "vdev_passivate",
		    sizeof (rec.zi_func));
		VERIFY0(zio_inject_fault(poolname, 0, &id, &rec));
	} else if (strcmp(how, "after_x") == 0) {
		zinject_record_t rec = { 0 };
		int id;

		rec.zi_cmd = ZINJECT_PANIC;
		(void) strlcpy(rec.zi_func, "spa_vdev_exit",
		    sizeof (rec.zi_func));
		VERIFY0(zio_inject_fault(poolname, 0, &id, &rec));
	} else {
		pthread_t t;

		watch_vdev_id = 1;
		VERIFY0(pthread_create(&t, NULL, watcher, spa));
		watch_arm = 1;
	}
	(void) do_remove(top_guid(1));
	(void) printf("    child: removal returned without crashing\n");
	_exit(3);
}

static void
sc_crash(const char *how, boolean_t reuse)
{
	int st;
	pid_t p;
	char cfgpath[1200];
	nvlist_t *cfg;
	vobjs_t vo;
	FILE *f;
	struct stat sb;
	char *buf;

	(void) fflush(stdout);
	p = fork();
	if (p == 0)
		crash_child(how);
	VERIFY3S(waitpid(p, &st, 0), ==, p);
	chk(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
	    "child died of SIGABRT inside the removal (status %#x)", st);

	kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	/*
	 * kernel_init() reads the cache file, as a boot would.  Opening the pool
	 * loads it from there; the saved config is only a fallback.
	 */
	{
		spa_t *spa;
		int e = spa_open(poolname, &spa, FTAG);

		if (e == 0)
			spa_close(spa, FTAG);
		if (e == ENOENT) {
			(void) snprintf(cfgpath, sizeof (cfgpath),
			    "%s/config.nv", dir);
			VERIFY0(stat(cfgpath, &sb));
			buf = malloc(sb.st_size);
			f = fopen(cfgpath, "r");
			VERIFY3U(fread(buf, 1, sb.st_size, f), ==, sb.st_size);
			(void) fclose(f);
			VERIFY0(nvlist_unpack(buf, sb.st_size, &cfg, 0));
			e = spa_import(poolname, cfg, NULL, 0);
		}
		chk(e == 0, "pool loads after the crash (%d)", e);
		if (e != 0)
			return;
	}
	tree("after crash + import");
	uint64_t children = top_field(0, "children");
	int nh = 0, nr = 0;

	config_view("after crash + import", &nh, &nr);
	if (nh == 1) {
		(void) printf("    state: the crash was after the txg that "
		    "destroyed the MOS objects, the hole is not written\n");
	}
	chk(children == 2, "two top-level vdevs");
	if (nh == 0) {
		chk(strcmp(top_type(1), "raidz") == 0,
		    "target is still a raidz");
		chk(top_field(1, "alloc") == 0, "target is still empty");
		chk(top_field(1, "removing") == 0 && nr == 0,
		    "no persisted removing state");
		{
			char path[1200];
			uint64_t lr, ln, lm;
			spa_t *sp;

			VERIFY0(spa_open(poolname, &sp, FTAG));
			spa_config_enter(sp, SCL_VDEV, FTAG, RW_READER);
			(void) strlcpy(path, sp->spa_root_vdev->vdev_child[1]->
			    vdev_child[0]->vdev_path, sizeof (path));
			spa_config_exit(sp, SCL_VDEV, FTAG);
			spa_close(sp, FTAG);
			chk(label_tree(path, &lr, &ln, &lm),
			    "label of a leaf of the target is readable");
			(void) printf("    label of the target: removing=%llu "
			    "noalloc=%llu ms_array=%llu\n",
			    (u_longlong_t)lr, (u_longlong_t)ln,
			    (u_longlong_t)lm);
			chk(lr == 0, "the label has no removing flag");
		}
		(void) printf("    state: noalloc=%llu ms_array=%llu "
		    "top_zap=%llu\n",
		    (u_longlong_t)top_field(1, "noalloc"),
		    (u_longlong_t)top_field(1, "ms_array"),
		    (u_longlong_t)top_field(1, "top_zap"));
		if (reuse) {
			spa_t *spa;
			int e2;

			VERIFY0(spa_open(poolname, &spa, FTAG));
			e2 = spa_vdev_alloc(spa, top_guid(1));
			spa_close(spa, FTAG);
			chk(e2 == 0, "allocating=on on the vdev left by the "
			    "crash (%d)", e2);
			chk(scrub_clean(), "scrub clean before the reuse");
			churn(10);
			tree("after reuse");
			(void) printf("    OBSERVE vdev alloc after reuse: "
			    "%llu, ms_array=%llu top_zap=%llu\n",
			    (u_longlong_t)top_field(1, "alloc"),
			    (u_longlong_t)top_field(1, "ms_array"),
			    (u_longlong_t)top_field(1, "top_zap"));
			chk(scrub_clean(), "scrub clean after reuse");
			chk(reimport() == 0, "export + import");
			chk(scrub_clean(), "scrub clean after import");
			leave_for_zdb();
			return;
		}
		get_objs(1, &vo);
		chk_err("retry of the removal", do_remove(top_guid(1)), 0, 0);
		chk(strcmp(top_type(1), "hole") == 0, "target is a hole");
		chk(objs_still_allocated(&vo) == 0,
		    "every MOS object of the vdev was freed (%d tracked)",
		    vo.n);
		{
			snap_t s;

			take_snap(&s);
			print_snap("after retry", &s);
			chk(s.nonalloc == 0 && s.root_space == s.n_space,
			    "space accounting is clean");
		}
		chk(reimport() == 0, "export + import");
		chk(scrub_clean(), "scrub clean after the retry");
	}
	leave_for_zdb();
}

static void sc_crash_passivate(void) { sc_crash("passivate", B_FALSE); }
static void sc_crash_commit(void) { sc_crash("commit", B_FALSE); }
static void sc_crash_after_x(void) { sc_crash("after_x", B_FALSE); }
static void sc_crash_after_x_reuse(void) { sc_crash("after_x", B_TRUE); }
#endif

typedef struct scenario {
	const char *name;
	void (*fn)(void);
	boolean_t manual_init;
	const char *desc;
} scenario_t;

static const scenario_t scenarios[] = {
	{ "create2", sc_create2, B_FALSE, "remove_raidz.ksh layout" },
	{ "raidz_add_raidz", sc_raidz_add_raidz, B_FALSE, "" },
	{ "raidz_add_raidz_noalloc", sc_raidz_add_raidz_noalloc, B_FALSE, "" },
	{ "raidz_add_disk", sc_raidz_add_disk, B_FALSE, "" },
	{ "raidz_add_mirror", sc_raidz_add_mirror, B_FALSE, "" },
	{ "raidz_add_special", sc_raidz_add_special, B_FALSE, "" },
	{ "raidz_add_dedup", sc_raidz_add_dedup, B_FALSE, "" },
	{ "mirror_add_raidz", sc_mirror_add_raidz, B_FALSE, "" },
	{ "draid_add_disk", sc_draid_add_disk, B_FALSE, "" },
	{ "draid_add_draid", sc_draid_add_draid, B_FALSE, "" },
	{ "mirror_add_draid", sc_mirror_add_draid, B_FALSE, "" },
	{ "noraidz_evacuation", sc_noraidz_evacuation, B_FALSE, "" },
	{ "mixed_noraidz", sc_mixed_noraidz, B_FALSE, "" },
	{ "mixed_raidz", sc_mixed_raidz, B_FALSE, "" },
	{ "populated", sc_populated, B_FALSE, "" },
	{ "churn_noalloc", sc_churn_noalloc, B_FALSE, "" },
	{ "checkpoint", sc_checkpoint, B_FALSE, "" },
	{ "expand", sc_expand, B_FALSE, "" },
	{ "expanded_empty", sc_expanded_empty, B_FALSE, "" },
	{ "feature_disabled", sc_feature_disabled, B_FALSE, "" },
	{ "log", sc_log, B_FALSE, "" },
	{ "cycles", sc_cycles, B_FALSE, "" },
	{ "degraded", sc_degraded, B_FALSE, "" },
	{ "scrub_running", sc_scrub_running, B_FALSE, "" },
	{ "initialize", sc_initialize, B_FALSE, "" },
	{ "trim", sc_trim, B_FALSE, "" },
	{ "history", sc_history, B_FALSE, "" },
	{ "history_sync", sc_history_sync, B_FALSE, "" },
	{ "expand_target", sc_expand_target, B_FALSE, "" },
	{ "stress", sc_stress, B_FALSE, "" },
#ifdef HAVE_REMOVING_EMPTY
	{ "crash_passivate", sc_crash_passivate, B_TRUE, "" },
	{ "crash_commit", sc_crash_commit, B_TRUE, "" },
	{ "crash_after_x", sc_crash_after_x, B_TRUE, "" },
	{ "crash_after_x_reuse", sc_crash_after_x_reuse, B_TRUE, "" },
#endif
	{ NULL, NULL, B_FALSE, NULL }
};

static int
run_one(const scenario_t *sc)
{
	char *cache;
	struct rlimit rl = { 0, 0 };

	(void) setrlimit(RLIMIT_CORE, &rl);
	(void) snprintf(poolname, sizeof (poolname), "p_%s", sc->name);
	(void) snprintf(dir, sizeof (dir), "%s/%s", root, sc->name);
	(void) mkdir(dir, 0755);
	nfile = 0;
	VERIFY3S(asprintf(&cache, "%s/zpool.cache", dir), !=, -1);
	spa_config_path = cache;
	(void) printf("=== %s (%s tree)\n", sc->name, is_new ? "new" : "old");
	if (!sc->manual_init)
		kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);
	sc->fn();
	kernel_fini();
	(void) printf("=== %s: %d pass, %d FAIL\n", sc->name, npass, nfail);
	return (nfail != 0 ? 1 : 0);
}

int
main(int argc, char **argv)
{
	int rc = 0, total_fail = 0;

	if (argc < 3)
		return (2);
	root = argv[1];
	is_new = strcmp(argv[2], "new") == 0;
	(void) setvbuf(stdout, NULL, _IOLBF, 0);

	for (const scenario_t *sc = scenarios; sc->name != NULL; sc++) {
		boolean_t selected = argc == 3;

		for (int i = 3; i < argc; i++)
			if (strcmp(argv[i], sc->name) == 0)
				selected = B_TRUE;
		if (!selected)
			continue;
		(void) fflush(stdout);
		pid_t p = fork();
		if (p == 0)
			_exit(run_one(sc));
		int st;
		VERIFY3S(waitpid(p, &st, 0), ==, p);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			(void) printf("=== %s: exit status %#x\n", sc->name,
			    st);
			total_fail++;
		}
	}
	(void) printf("SUMMARY scenarios with failures or aborts: %d\n",
	    total_fail);
	return (rc);
}
