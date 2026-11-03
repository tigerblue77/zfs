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
 * Map equivalence check between a never-expanded RAID-Z vdev and an
 * anyraidz vdev of the same width and parity whose tiles are identity
 * mapped onto the RAID-Z layout (tile k is slot k of every child, in child
 * order, slots starting at offset 0 of each child).
 *
 * For every block (offset, psize) tried, both maps are built with the real
 * libzpool code: vdev_raidz_map_alloc() for RAID-Z, the same function
 * followed by vdev_anyraid_raidz_map_translate() for anyraidz.  The two
 * maps are compared column by column (child, offset, size, and for writes
 * the size of the I/O).  The psize to asize functions and the xlate
 * functions of both vdev types (used by initialize, TRIM and the debug I/O
 * verification) are compared too.
 *
 * A block is classified by whether its allocation [offset, offset + asize)
 * lies inside one logical tile (tile size times width).  Mismatches inside
 * one tile are failures of the identity; mismatches of blocks that cross a
 * tile boundary are expected and are counted as counter-examples, together
 * with whether such a block could exist, i.e. whether it lies inside one
 * metaslab.
 */

#include <sys/zfs_context.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_raidz_impl.h>
#include <sys/vdev_anyraid.h>
#include <sys/vdev_anyraid_impl.h>
#include <sys/zio.h>
#include <sys/abd.h>
#include <libzpool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct cfg {
	uint64_t w;
	uint64_t p;
	uint64_t ashift;
	uint64_t tile_shift;	/* per child tile size, log2 */
	uint64_t ms_shift;
	uint64_t ntiles;
	int mode;		/* 'e'xhaustive, 'b'oundary, 'r'andom */
	uint64_t count;		/* random samples or boundary span */
	uint64_t maxsec;	/* largest psize tried, in sectors */
	uint64_t seed;
	boolean_t writes;
	boolean_t std_base;	/* control: keep the standard slot base */
	int verbose;
} cfg_t;

typedef struct stats {
	uint64_t blocks;
	uint64_t in_tile;
	uint64_t in_tile_map_bad;
	uint64_t in_tile_xlate_bad;
	uint64_t in_tile_in_ms;
	uint64_t in_tile_in_ms_map_bad;
	uint64_t in_tile_in_ms_xlate_bad;
	uint64_t cross_tile;
	uint64_t cross_tile_map_bad;
	uint64_t cross_tile_in_ms;
	uint64_t cross_tile_in_ms_map_bad;
	uint64_t asize_bad;
	uint64_t psize_bad;
	uint64_t printed_map;
	uint64_t printed_xlate;
	uint64_t printed_xlate_lines;
	uint64_t printed_cross;
} stats_t;

static cfg_t cfg;
static stats_t st;
static spa_t *fake_spa;
static vdev_t *rvd, *avd;
static abd_t *big_abd;

static vdev_t *
fake_top(vdev_ops_t *ops, uint64_t w, uint64_t p, boolean_t anyraid)
{
	nvlist_t *nv = fnvlist_alloc();
	nvlist_t **child = kmem_alloc(w * sizeof (nvlist_t *), KM_SLEEP);
	for (uint64_t c = 0; c < w; c++) {
		child[c] = fnvlist_alloc();
		fnvlist_add_string(child[c], ZPOOL_CONFIG_TYPE,
		    VDEV_TYPE_FILE);
	}
	fnvlist_add_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    (const nvlist_t * const *)child, w);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_NPARITY, p);
	if (anyraid) {
		fnvlist_add_uint8(nv, ZPOOL_CONFIG_ANYRAID_PARITY_TYPE,
		    VAP_RAIDZ);
		fnvlist_add_uint8(nv, ZPOOL_CONFIG_ANYRAID_NDATA, w - p);
	}

	void *tsd = NULL;
	VERIFY0(ops->vdev_op_init(fake_spa, nv, &tsd));
	for (uint64_t c = 0; c < w; c++)
		fnvlist_free(child[c]);
	kmem_free(child, w * sizeof (nvlist_t *));
	fnvlist_free(nv);

	vdev_t *vd = kmem_zalloc(sizeof (vdev_t), KM_SLEEP);
	vd->vdev_ops = ops;
	vd->vdev_tsd = tsd;
	vd->vdev_top = vd;
	vd->vdev_ashift = cfg.ashift;
	vd->vdev_children = w;
	vd->vdev_child = kmem_zalloc(w * sizeof (vdev_t *), KM_SLEEP);
	for (uint64_t c = 0; c < w; c++) {
		vdev_t *cvd = kmem_zalloc(sizeof (vdev_t), KM_SLEEP);
		cvd->vdev_ops = &vdev_file_ops;
		cvd->vdev_parent = vd;
		cvd->vdev_top = vd;
		cvd->vdev_id = c;
		cvd->vdev_ashift = cfg.ashift;
		vd->vdev_child[c] = cvd;
	}
	return (vd);
}

static void
print_map(const char *tag, raidz_map_t *rm)
{
	raidz_row_t *rr = rm->rm_row[0];
	(void) printf("    %s: cols %d scols %d bigcols %d nskip %d "
	    "skipstart %d\n", tag, (int)rr->rr_cols, (int)rr->rr_scols,
	    (int)rr->rr_bigcols, (int)rm->rm_nskip, (int)rm->rm_skipstart);
	for (int c = 0; c < rr->rr_scols; c++) {
		raidz_col_t *rc = &rr->rr_col[c];
		(void) printf("      col %d: child %d offset 0x%llx size "
		    "0x%llx\n", c, (int)rc->rc_devidx,
		    (u_longlong_t)rc->rc_offset, (u_longlong_t)rc->rc_size);
	}
}

/*
 * Compare two maps; return B_TRUE when they choose the same sectors.
 */
static boolean_t
maps_equal(raidz_map_t *a, raidz_map_t *b, boolean_t write)
{
	raidz_row_t *ra = a->rm_row[0], *rb = b->rm_row[0];
	if (ra->rr_cols != rb->rr_cols || ra->rr_scols != rb->rr_scols ||
	    ra->rr_bigcols != rb->rr_bigcols ||
	    a->rm_nskip != b->rm_nskip || a->rm_skipstart != b->rm_skipstart)
		return (B_FALSE);
	for (int c = 0; c < ra->rr_scols; c++) {
		raidz_col_t *ca = &ra->rr_col[c], *cb = &rb->rr_col[c];
		if (ca->rc_devidx != cb->rc_devidx ||
		    ca->rc_offset != cb->rc_offset ||
		    ca->rc_size != cb->rc_size)
			return (B_FALSE);
		if (write && (ca->rc_abd == NULL) != (cb->rc_abd == NULL))
			return (B_FALSE);
		if (write && ca->rc_abd != NULL &&
		    abd_get_size(ca->rc_abd) != abd_get_size(cb->rc_abd))
			return (B_FALSE);
	}
	return (B_TRUE);
}

static void
check_block(uint64_t b, uint64_t s, int type)
{
	uint64_t ashift = cfg.ashift;
	uint64_t offset = b << ashift;
	uint64_t psize = s << ashift;
	uint64_t lt = (1ULL << cfg.tile_shift) * cfg.w;
	uint64_t ms = 1ULL << cfg.ms_shift;
	vdev_anyraid_t *va = avd->vdev_tsd;

	uint64_t asize_r = vdev_raidz_ops.vdev_op_psize_to_asize(rvd, psize,
	    1);
	uint64_t asize_a = vdev_anyraidz_ops.vdev_op_psize_to_asize(avd,
	    psize, 1);
	if (offset + asize_r > lt * cfg.ntiles)
		return;
	st.blocks++;
	if (asize_r != asize_a)
		st.asize_bad++;
	if (vdev_raidz_ops.vdev_op_asize_to_psize(rvd, asize_r, 1) !=
	    vdev_anyraidz_ops.vdev_op_asize_to_psize(avd, asize_r, 1))
		st.psize_bad++;

	boolean_t in_tile = (offset / lt == (offset + asize_r - 1) / lt);
	boolean_t in_ms = (offset / ms == (offset + asize_r - 1) / ms);

	zio_t zio = { 0 };
	zio.io_offset = offset;
	zio.io_size = psize;
	zio.io_type = type;
	zio.io_abd = big_abd;

	raidz_map_t *rm_r = vdev_raidz_map_alloc(&zio, ashift, cfg.w, cfg.p);
	raidz_map_t *rm_a = vdev_raidz_map_alloc(&zio, ashift, cfg.w, cfg.p);
	anyraid_tile_t *tile = vdev_anyraid_find_tile(va, offset);
	VERIFY(tile != NULL);
	vdev_anyraid_raidz_map_translate(avd, rm_a, tile, NULL);
	boolean_t same = maps_equal(rm_r, rm_a, type == ZIO_TYPE_WRITE);

	boolean_t xsame = B_TRUE;
	if (in_tile) {
		for (uint64_t c = 0; c < cfg.w; c++) {
			zfs_range_seg64_t l, pr, pa, rr, ra;
			l.rs_start = offset;
			l.rs_end = offset + asize_r;
			pr.rs_start = pr.rs_end = pa.rs_start = pa.rs_end = 0;
			rr.rs_start = rr.rs_end = ra.rs_start = ra.rs_end = 0;
			vdev_raidz_ops.vdev_op_xlate(rvd->vdev_child[c], &l,
			    &pr, &rr);
			vdev_anyraidz_ops.vdev_op_xlate(avd->vdev_child[c],
			    &l, &pa, &ra);
			if (pr.rs_start != pa.rs_start ||
			    pr.rs_end != pa.rs_end) {
				xsame = B_FALSE;
				if (cfg.verbose &&
				    st.printed_xlate_lines++ < 20) {
					(void) printf("  xlate child %llu of "
					    "[0x%llx, 0x%llx): raidz "
					    "[0x%llx, 0x%llx) anyraid "
					    "[0x%llx, 0x%llx)\n",
					    (u_longlong_t)c,
					    (u_longlong_t)l.rs_start,
					    (u_longlong_t)l.rs_end,
					    (u_longlong_t)pr.rs_start,
					    (u_longlong_t)pr.rs_end,
					    (u_longlong_t)pa.rs_start,
					    (u_longlong_t)pa.rs_end);
				}
			}
		}
	}

	if (in_tile) {
		st.in_tile++;
		if (!same)
			st.in_tile_map_bad++;
		if (!xsame)
			st.in_tile_xlate_bad++;
		if (in_ms) {
			st.in_tile_in_ms++;
			if (!same)
				st.in_tile_in_ms_map_bad++;
			if (!xsame)
				st.in_tile_in_ms_xlate_bad++;
		}
	} else {
		st.cross_tile++;
		if (!same)
			st.cross_tile_map_bad++;
		if (in_ms) {
			st.cross_tile_in_ms++;
			if (!same)
				st.cross_tile_in_ms_map_bad++;
		}
	}
	uint64_t *printed = !in_tile ? &st.printed_cross :
	    !same ? &st.printed_map : &st.printed_xlate;
	if ((!same || !xsame) && cfg.verbose && *printed < 3 &&
	    (in_ms || cfg.verbose > 1)) {
		(*printed)++;
		(void) printf("  %s mismatch%s: offset 0x%llx psize 0x%llx "
		    "asize 0x%llx tile %llu %s\n",
		    in_tile ? "IN-TILE" : "cross-tile", same ? " (xlate)" : "",
		    (u_longlong_t)offset, (u_longlong_t)psize,
		    (u_longlong_t)asize_r, (u_longlong_t)(offset / lt),
		    in_ms ? "inside one metaslab" : "crosses a metaslab");
		if (!same) {
			print_map("raidz  ", rm_r);
			print_map("anyraid", rm_a);
		}
	}
	vdev_raidz_map_free(rm_r);
	vdev_raidz_map_free(rm_a);
}

static void
check_both(uint64_t b, uint64_t s)
{
	check_block(b, s, ZIO_TYPE_READ);
	if (cfg.writes)
		check_block(b, s, ZIO_TYPE_WRITE);
}

static uint64_t
rnd(void)
{
	/* splitmix64 */
	uint64_t z = (cfg.seed += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return (z ^ (z >> 31));
}

static void
usage(void)
{
	(void) fprintf(stderr, "usage: anyraid_adopt_map -w width -p parity "
	    "-a ashift -t tile_shift -m ms_shift -n ntiles\n"
	    "\t[-e | -b span | -r count] [-x maxsec] [-s seed] [-W] [-S] "
	    "[-v]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	int c;
	cfg.mode = 'e';
	cfg.seed = 1;
	cfg.ashift = 9;
	while ((c = getopt(argc, argv, "w:p:a:t:m:n:eb:r:x:s:WSv")) != -1) {
		switch (c) {
		case 'w': cfg.w = strtoull(optarg, NULL, 0); break;
		case 'p': cfg.p = strtoull(optarg, NULL, 0); break;
		case 'a': cfg.ashift = strtoull(optarg, NULL, 0); break;
		case 't': cfg.tile_shift = strtoull(optarg, NULL, 0); break;
		case 'm': cfg.ms_shift = strtoull(optarg, NULL, 0); break;
		case 'n': cfg.ntiles = strtoull(optarg, NULL, 0); break;
		case 'e': cfg.mode = 'e'; break;
		case 'b': cfg.mode = 'b';
			cfg.count = strtoull(optarg, NULL, 0); break;
		case 'r': cfg.mode = 'r';
			cfg.count = strtoull(optarg, NULL, 0); break;
		case 'x': cfg.maxsec = strtoull(optarg, NULL, 0); break;
		case 's': cfg.seed = strtoull(optarg, NULL, 0); break;
		case 'W': cfg.writes = B_TRUE; break;
		case 'S': cfg.std_base = B_TRUE; break;
		case 'v': cfg.verbose++; break;
		default: usage();
		}
	}
	if (cfg.w == 0 || cfg.p == 0 || cfg.p >= cfg.w || cfg.tile_shift == 0 ||
	    cfg.ms_shift == 0 || cfg.ntiles == 0)
		usage();
	if (cfg.maxsec == 0)
		cfg.maxsec = SPA_MAXBLOCKSIZE >> cfg.ashift;

	kernel_init(SPA_MODE_READ);
	fake_spa = kmem_zalloc(sizeof (spa_t), KM_SLEEP);
	fake_spa->spa_ubsync.ub_version = SPA_VERSION;
	big_abd = abd_alloc_linear(SPA_MAXBLOCKSIZE, B_FALSE);
	abd_zero(big_abd, SPA_MAXBLOCKSIZE);

	rvd = fake_top(&vdev_raidz_ops, cfg.w, cfg.p, B_FALSE);
	avd = fake_top(&vdev_anyraidz_ops, cfg.w, cfg.p, B_TRUE);
	vdev_anyraid_t *va = avd->vdev_tsd;
	va->vd_tile_size = 1ULL << cfg.tile_shift;
	va->vd_legacy = B_TRUE;
	vdev_anyraid_legacy_tiles(va, cfg.ntiles);
	if (cfg.std_base)
		va->vd_legacy = B_FALSE;

	uint64_t sps = 1ULL << (cfg.tile_shift - cfg.ashift);
	uint64_t ltsec = sps * cfg.w;
	uint64_t total = ltsec * cfg.ntiles;

	(void) printf("w=%llu p=%llu ashift=%llu tile=2^%llu per child "
	    "(logical tile 0x%llx) ms=2^%llu ntiles=%llu mode=%c%s%s\n",
	    (u_longlong_t)cfg.w, (u_longlong_t)cfg.p,
	    (u_longlong_t)cfg.ashift, (u_longlong_t)cfg.tile_shift,
	    (u_longlong_t)(ltsec << cfg.ashift),
	    (u_longlong_t)cfg.ms_shift, (u_longlong_t)cfg.ntiles, cfg.mode,
	    cfg.writes ? " +writes" : "",
	    cfg.std_base ? " CONTROL(standard slot base)" : "");
	(void) printf("logical tile is %s multiple of the metaslab size\n",
	    ((ltsec << cfg.ashift) % (1ULL << cfg.ms_shift)) == 0 ?
	    "a" : "NOT a");

	switch (cfg.mode) {
	case 'e':
		for (uint64_t b = 0; b < total; b++) {
			for (uint64_t s = 1; s <= cfg.maxsec &&
			    b + s <= total; s++)
				check_both(b, s);
		}
		break;
	case 'b': {
		/*
		 * Every offset within span sectors before and after each
		 * tile boundary, with psizes of 1 to 64 sectors, every
		 * power of two and its neighbours, and 16 random ones.
		 */
		uint64_t sizes[256];
		int ns = 0;
		for (uint64_t s = 1; s <= 64 && s <= cfg.maxsec; s++)
			sizes[ns++] = s;
		for (uint64_t s = 128; s <= cfg.maxsec; s <<= 1) {
			sizes[ns++] = s - 1;
			sizes[ns++] = s;
			if (s + 1 <= cfg.maxsec)
				sizes[ns++] = s + 1;
		}
		for (int i = 0; i < 16; i++)
			sizes[ns++] = 1 + rnd() % cfg.maxsec;
		for (uint64_t t = 1; t <= cfg.ntiles; t++) {
			uint64_t end = t * ltsec;
			uint64_t lo = end > cfg.count ? end - cfg.count : 0;
			uint64_t hi = MIN(end + cfg.count, total);
			for (uint64_t b = lo; b < hi; b++)
				for (int i = 0; i < ns; i++)
					check_both(b, sizes[i]);
			/* Blocks that end near the boundary. */
			for (int i = 0; i < ns; i++) {
				uint64_t as = vdev_raidz_ops.
				    vdev_op_psize_to_asize(rvd,
				    sizes[i] << cfg.ashift, 1) >> cfg.ashift;
				if (as + cfg.count > end)
					continue;
				for (uint64_t b = end - as - cfg.count;
				    b < end - as + cfg.count; b++) {
					if (b >= lo && b < hi)
						continue;
					check_both(b, sizes[i]);
				}
			}
		}
		break;
	}
	case 'r':
		for (uint64_t i = 0; i < cfg.count; i++) {
			uint64_t b = rnd() % total;
			uint64_t s = 1 + rnd() % cfg.maxsec;
			check_both(b, s);
		}
		break;
	}

	(void) printf("blocks %llu; asize mismatches %llu, psize mismatches "
	    "%llu\n", (u_longlong_t)st.blocks, (u_longlong_t)st.asize_bad,
	    (u_longlong_t)st.psize_bad);
	(void) printf("  inside one tile: %llu, map mismatches %llu, xlate "
	    "mismatches %llu\n", (u_longlong_t)st.in_tile,
	    (u_longlong_t)st.in_tile_map_bad,
	    (u_longlong_t)st.in_tile_xlate_bad);
	(void) printf("    of which inside one metaslab: %llu, map mismatches "
	    "%llu, xlate mismatches %llu\n", (u_longlong_t)st.in_tile_in_ms,
	    (u_longlong_t)st.in_tile_in_ms_map_bad,
	    (u_longlong_t)st.in_tile_in_ms_xlate_bad);
	(void) printf("  crossing a tile boundary: %llu, map mismatches %llu; "
	    "inside one metaslab (blocks that can exist): %llu, map mismatches "
	    "%llu\n", (u_longlong_t)st.cross_tile,
	    (u_longlong_t)st.cross_tile_map_bad,
	    (u_longlong_t)st.cross_tile_in_ms,
	    (u_longlong_t)st.cross_tile_in_ms_map_bad);

	boolean_t ok = (st.in_tile_map_bad == 0 && st.in_tile_xlate_bad == 0 &&
	    st.asize_bad == 0 && st.psize_bad == 0 && st.cross_tile_in_ms == 0);
	(void) printf("%s\n", ok ? "IDENTITY HOLDS for every block that can "
	    "exist" : "IDENTITY FAILS");
	abd_free(big_abd);
	kernel_fini();
	return (ok ? 0 : 1);
}
