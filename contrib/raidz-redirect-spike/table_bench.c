// SPDX-License-Identifier: CDDL-1.0
/*
 * Spike step 7 (throwaway): which in-memory index can hold the redirect
 * table of one raidz vdev at 1.7e7 entries within 1 GiB and answer the I/O
 * path's lookup in 1 microsecond (p99)?  This is the choice experiment
 * only: it builds each candidate outside of any pool, with libzpool's own
 * avl, zfs_btree and locks, and the same synthetic keys as scale_probe.c.
 * The numbers that count are scale_probe's, on the module's own lookup.
 *
 *   table_bench <avl|bt48|bt40|hash> <n> [threads] [rw|rrm|none]
 *
 * avl:  the node of the current design, keyed by {offset, birth} (120
 *       bytes in one allocation per entry).
 * bt48: a zfs_btree_t of 48-byte elements keyed by offset alone (the kept
 *       head sector makes the old offset unique among live entries).
 * bt40: the same with a 40-byte element (new birth as a 32-bit delta).
 * hash: an open-addressing Robin Hood table of the 40-byte element, sized
 *       for a load factor of 0.8, keyed by offset.
 *
 * Keys: <n> distinct odd 512-byte sectors of a 32 GiB space, so that no
 * parity-1 block, whose extents start at even sectors, starts at one; the
 * births are old (2 to 999), the new births later (1000 to 1500).  The
 * index is built in random order (as from a log in move order), then for
 * btrees again in key order (bulk).  Lookups: 1e6 hits (random existing
 * keys) and 1e6 misses (random even sectors), each timed alone with
 * clock_gettime(CLOCK_MONOTONIC), under the read side of the lock, by one
 * thread, then by <threads> threads at once.  Memory: mallinfo2() before
 * and after the build (malloc's in-use bytes, chunk headers included).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <time.h>
#include <sys/zfs_context.h>
#include <sys/avl.h>
#include <sys/btree.h>
#include <sys/rrwlock.h>
#include <sys/spa.h>
#include <libzpool.h>

#define	NQ		1000000
#define	SPACE_SECTORS	(1ULL << 26)	/* 32 GiB of 512-byte sectors */
#define	EPOCH		1000

typedef struct ent7 {
	uint64_t	e_offset;
	uint64_t	e_birth;
	uint64_t	e_new_offset;
	uint64_t	e_new_birth;
	uint64_t	e_new_asize;
	uint64_t	e_old_reserved;
	uint64_t	e_old_asize;
} ent7_t;

/* The node of the current design (vdev_raidz.c). */
typedef struct avl_node_ent {
	avl_node_t	an_link;
	list_node_t	an_pending;
	ent7_t		an_ent;
	boolean_t	an_tomb;
	uint64_t	an_free_txg;
	uint64_t	an_free_pass;
} avl_node_ent_t;

typedef struct el48 {
	uint64_t	b_offset;
	uint64_t	b_birth;
	uint64_t	b_new_offset;
	uint64_t	b_new_birth;
	uint32_t	b_new_asize;	/* 512-byte units */
	uint32_t	b_old_asize;
	uint32_t	b_old_reserved;
	uint32_t	b_aux;
} el48_t;

typedef struct el40 {
	uint64_t	c_offset;
	uint64_t	c_birth;
	uint64_t	c_new_offset;
	uint32_t	c_new_birth_delta;
	uint32_t	c_new_asize;
	uint32_t	c_old_asize;
	uint32_t	c_old_reserved;
} el40_t;

typedef struct query {
	uint64_t	q_offset;
	uint64_t	q_birth;
} query_t;

static int kind;	/* 0 avl, 1 bt48, 2 bt40, 3 hash */
static avl_tree_t avl;
static zfs_btree_t bt;
static el40_t *ht;
static uint64_t ht_cap;
static krwlock_t rwl;
static rrmlock_t rrm;
static int lockmode;	/* 0 rw, 1 rrm, 2 none */

static uint64_t
xs(uint64_t *s)
{
	uint64_t x = *s;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return (x);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

static size_t
heap_used(void)
{
	struct mallinfo2 mi = mallinfo2();

	return (mi.uordblks + mi.hblkhd);
}

static int
avl_cmp(const void *a, const void *b)
{
	const ent7_t *x = &((const avl_node_ent_t *)a)->an_ent;
	const ent7_t *y = &((const avl_node_ent_t *)b)->an_ent;
	int c = TREE_CMP(x->e_offset, y->e_offset);

	return (c != 0 ? c : TREE_CMP(x->e_birth, y->e_birth));
}

static int
b48_cmp(const void *a, const void *b)
{
	return (TREE_CMP(((const el48_t *)a)->b_offset,
	    ((const el48_t *)b)->b_offset));
}

static int
b40_cmp(const void *a, const void *b)
{
	return (TREE_CMP(((const el40_t *)a)->c_offset,
	    ((const el40_t *)b)->c_offset));
}

ZFS_BTREE_FIND_IN_BUF_FUNC(b48_find_in_buf, el48_t, b48_cmp)
ZFS_BTREE_FIND_IN_BUF_FUNC(b40_find_in_buf, el40_t, b40_cmp)

static uint64_t
hslot(uint64_t offset)
{
	uint64_t h = offset;

	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return ((uint64_t)(((__uint128_t)h * ht_cap) >> 64));
}

static void
ht_insert(el40_t e)
{
	uint64_t i = hslot(e.c_offset);
	uint64_t d = 0;

	for (;;) {
		el40_t *s = &ht[i];
		uint64_t sd;

		if (s->c_birth == 0) {
			*s = e;
			return;
		}
		sd = (i + ht_cap - hslot(s->c_offset)) % ht_cap;
		if (sd < d) {
			el40_t t = *s;

			*s = e;
			e = t;
			d = sd;
		}
		i = i + 1 == ht_cap ? 0 : i + 1;
		d++;
	}
}

static const el40_t *
ht_find(uint64_t offset)
{
	uint64_t i = hslot(offset);
	uint64_t d = 0;

	for (;;) {
		const el40_t *s = &ht[i];

		if (s->c_birth == 0)
			return (NULL);
		if (s->c_offset == offset)
			return (s);
		if ((i + ht_cap - hslot(s->c_offset)) % ht_cap < d)
			return (NULL);
		i = i + 1 == ht_cap ? 0 : i + 1;
		d++;
	}
}

static void
make_ent(uint64_t sector, uint64_t *seed, ent7_t *e)
{
	e->e_offset = sector << 9;
	e->e_birth = 2 + xs(seed) % (EPOCH - 2);
	e->e_new_offset = ((xs(seed) % SPACE_SECTORS) & ~1ULL) << 9;
	e->e_new_birth = EPOCH + xs(seed) % 500;
	e->e_new_asize = 429 << 9;
	e->e_old_reserved = 1 << 9;
	e->e_old_asize = 320 << 9;
}

static void
insert(const ent7_t *e)
{
	if (kind == 0) {
		avl_node_ent_t *n = kmem_zalloc(sizeof (*n), KM_SLEEP);

		n->an_ent = *e;
		avl_add(&avl, n);
	} else if (kind == 1) {
		el48_t b = { e->e_offset, e->e_birth, e->e_new_offset,
		    e->e_new_birth, e->e_new_asize >> 9, e->e_old_asize >> 9,
		    e->e_old_reserved >> 9, 0 };

		zfs_btree_add(&bt, &b);
	} else {
		el40_t c = { e->e_offset, e->e_birth, e->e_new_offset,
		    (uint32_t)(e->e_new_birth - e->e_birth),
		    e->e_new_asize >> 9, e->e_old_asize >> 9,
		    e->e_old_reserved >> 9 };

		if (kind == 2)
			zfs_btree_add(&bt, &c);
		else
			ht_insert(c);
	}
}

/* The lookup as the I/O path makes it: lock, find, copy out, unlock. */
static boolean_t
lookup(uint64_t offset, uint64_t birth, ent7_t *out)
{
	boolean_t found = B_FALSE;

	if (lockmode == 0)
		rw_enter(&rwl, RW_READER);
	else if (lockmode == 1)
		rrm_enter_read(&rrm, FTAG);
	if (kind == 0) {
		avl_node_ent_t s, *n;

		s.an_ent.e_offset = offset;
		s.an_ent.e_birth = birth;
		n = avl_find(&avl, &s, NULL);
		if (n != NULL) {
			*out = n->an_ent;
			found = B_TRUE;
		}
	} else if (kind == 1) {
		el48_t s = { .b_offset = offset }, *b;

		b = zfs_btree_find(&bt, &s, NULL);
		if (b != NULL && b->b_birth == birth) {
			out->e_offset = b->b_offset;
			out->e_birth = b->b_birth;
			out->e_new_offset = b->b_new_offset;
			out->e_new_birth = b->b_new_birth;
			out->e_new_asize = (uint64_t)b->b_new_asize << 9;
			out->e_old_asize = (uint64_t)b->b_old_asize << 9;
			out->e_old_reserved = (uint64_t)b->b_old_reserved << 9;
			found = B_TRUE;
		}
	} else {
		el40_t s = { .c_offset = offset };
		const el40_t *c = kind == 2 ? zfs_btree_find(&bt, &s, NULL) :
		    ht_find(offset);

		if (c != NULL && c->c_birth == birth) {
			out->e_offset = c->c_offset;
			out->e_birth = c->c_birth;
			out->e_new_offset = c->c_new_offset;
			out->e_new_birth = c->c_birth + c->c_new_birth_delta;
			out->e_new_asize = (uint64_t)c->c_new_asize << 9;
			out->e_old_asize = (uint64_t)c->c_old_asize << 9;
			out->e_old_reserved = (uint64_t)c->c_old_reserved << 9;
			found = B_TRUE;
		}
	}
	if (lockmode == 0)
		rw_exit(&rwl);
	else if (lockmode == 1)
		rrm_exit(&rrm, FTAG);
	return (found);
}

typedef struct run {
	const query_t	*r_q;
	uint64_t	*r_lat;
	uint64_t	r_n;
	boolean_t	r_hit;
	uint64_t	r_bad;
	uint64_t	r_ns;
} run_t;

static void *
run_queries(void *arg)
{
	run_t *r = arg;
	uint64_t t0 = now_ns();

	for (uint64_t i = 0; i < r->r_n; i++) {
		ent7_t e;
		uint64_t a = now_ns();
		boolean_t f = lookup(r->r_q[i].q_offset, r->r_q[i].q_birth, &e);

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
report(const char *what, uint64_t *lat, uint64_t n, uint64_t ns,
    uint64_t bad, int threads)
{
	qsort(lat, n, sizeof (uint64_t), u64cmp);
	(void) printf("MEASURE lookup %s threads=%d n=%llu p50=%llu p90=%llu "
	    "p99=%llu p999=%llu max=%llu ns mean=%.1f ns wrong=%llu "
	    "wall=%.3f s\n", what, threads, (u_longlong_t)n,
	    (u_longlong_t)lat[n / 2], (u_longlong_t)lat[n * 9 / 10],
	    (u_longlong_t)lat[n * 99 / 100], (u_longlong_t)lat[n * 999 / 1000],
	    (u_longlong_t)lat[n - 1], (double)ns / (n / threads),
	    (u_longlong_t)bad, ns / 1e9);
}

static void
bench(const query_t *q, boolean_t hit, int threads)
{
	run_t r[16];
	pthread_t tid[16];
	uint64_t *lat = malloc(sizeof (uint64_t) * NQ * threads);
	uint64_t ns = 0, bad = 0;

	for (int t = 0; t < threads; t++) {
		r[t].r_q = q + (uint64_t)t * NQ;
		r[t].r_lat = lat + (uint64_t)t * NQ;
		r[t].r_n = NQ;
		r[t].r_hit = hit;
		r[t].r_bad = 0;
		r[t].r_ns = 0;
		VERIFY0(pthread_create(&tid[t], NULL, run_queries, &r[t]));
	}
	for (int t = 0; t < threads; t++) {
		VERIFY0(pthread_join(tid[t], NULL));
		ns = MAX(ns, r[t].r_ns);
		bad += r[t].r_bad;
	}
	report(hit ? "hit" : "miss", lat, (uint64_t)NQ * threads, ns, bad,
	    threads);
	free(lat);
}

static void
destroy(void)
{
	if (kind == 0) {
		void *c = NULL;
		avl_node_ent_t *n;

		while ((n = avl_destroy_nodes(&avl, &c)) != NULL)
			kmem_free(n, sizeof (*n));
	} else if (kind == 3) {
		free(ht);
		ht = NULL;
	} else {
		zfs_btree_clear(&bt);
	}
}

static void
create(void)
{
	if (kind == 0)
		avl_create(&avl, avl_cmp, sizeof (avl_node_ent_t),
		    offsetof(avl_node_ent_t, an_link));
	else if (kind == 1)
		zfs_btree_create(&bt, b48_cmp, b48_find_in_buf,
		    sizeof (el48_t));
	else if (kind == 2)
		zfs_btree_create(&bt, b40_cmp, b40_find_in_buf,
		    sizeof (el40_t));
}

static void
build(const uint64_t *sectors, uint64_t n, const char *order)
{
	uint64_t seed = 12345;
	size_t m0, m1;
	uint64_t t0, t1;

	m0 = heap_used();
	t0 = now_ns();
	create();
	if (kind == 3) {
		ht_cap = n * 5 / 4;
		ht = calloc(ht_cap, sizeof (el40_t));
	}
	for (uint64_t i = 0; i < n; i++) {
		ent7_t e;

		make_ent(sectors[i], &seed, &e);
		insert(&e);
	}
	t1 = now_ns();
	m1 = heap_used();
	(void) printf("MEASURE build %s n=%llu time=%.3f s (%.1f ns/entry) "
	    "heap=%zu bytes (%.1f bytes/entry)\n", order, (u_longlong_t)n,
	    (t1 - t0) / 1e9, (double)(t1 - t0) / n, m1 - m0,
	    (double)(m1 - m0) / n);
	if (kind == 1 || kind == 2) {
		(void) printf("INFO btree height=%d nodes=%llu leaf_cap=%u "
		    "elems=%llu\n", bt.bt_height,
		    (u_longlong_t)bt.bt_num_nodes, bt.bt_leaf_cap,
		    (u_longlong_t)bt.bt_num_elems);
	}
}

int
main(int argc, char **argv)
{
	static const char *const kinds[] = { "avl", "bt48", "bt40", "hash" };
	uint64_t n, *sectors, *shuffled, seed = 777, timer = 0, got = 0;
	uint8_t *bitmap;
	query_t *hits, *misses;
	int threads = 4;

	if (argc < 3) {
		(void) fprintf(stderr, "usage: table_bench "
		    "<avl|bt48|bt40|hash> <n> [threads] [rw|rrm|none]\n");
		return (2);
	}
	for (kind = 0; kind < 4 && strcmp(argv[1], kinds[kind]) != 0; kind++)
		;
	VERIFY3S(kind, <, 4);
	n = strtoull(argv[2], NULL, 0);
	if (argc > 3)
		threads = atoi(argv[3]);
	if (argc > 4)
		lockmode = strcmp(argv[4], "rw") == 0 ? 0 :
		    strcmp(argv[4], "rrm") == 0 ? 1 : 2;
	VERIFY3U(n, <, SPACE_SECTORS / 2);
	kernel_init(SPA_MODE_READ);
	rw_init(&rwl, NULL, RW_DEFAULT, NULL);
	rrm_init(&rrm, B_FALSE);

	/* n distinct odd sectors, in key order, then shuffled. */
	bitmap = calloc(SPACE_SECTORS / 2 / 8, 1);
	while (got < n) {
		uint64_t r = xs(&seed) % (SPACE_SECTORS / 2);

		if (!(bitmap[r / 8] & (1 << (r % 8)))) {
			bitmap[r / 8] |= 1 << (r % 8);
			got++;
		}
	}
	sectors = malloc(n * sizeof (uint64_t));
	shuffled = malloc(n * sizeof (uint64_t));
	got = 0;
	for (uint64_t r = 0; r < SPACE_SECTORS / 2; r++) {
		if (bitmap[r / 8] & (1 << (r % 8)))
			sectors[got++] = 2 * r + 1;
	}
	free(bitmap);
	memcpy(shuffled, sectors, n * sizeof (uint64_t));
	for (uint64_t i = n - 1; i > 0; i--) {
		uint64_t j = xs(&seed) % (i + 1), t = shuffled[i];

		shuffled[i] = shuffled[j];
		shuffled[j] = t;
	}
	(void) printf("INFO kind=%s n=%llu threads=%d lock=%s entry_in=%zu\n",
	    kinds[kind], (u_longlong_t)n, threads,
	    lockmode == 0 ? "rw" : lockmode == 1 ? "rrm" : "none",
	    kind == 0 ? sizeof (avl_node_ent_t) : kind == 1 ?
	    sizeof (el48_t) : sizeof (el40_t));

	/* Queries before the build, so that they do not count as table. */
	hits = malloc(sizeof (query_t) * NQ * threads);
	misses = malloc(sizeof (query_t) * NQ * threads);
	{
		uint64_t s2 = 12345;

		/* Replay make_ent()'s births for the hit keys. */
		uint64_t *births = malloc(n * sizeof (uint64_t));
		for (uint64_t i = 0; i < n; i++) {
			ent7_t e;

			make_ent(shuffled[i], &s2, &e);
			births[i] = e.e_birth;
		}
		for (uint64_t i = 0; i < (uint64_t)NQ * threads; i++) {
			uint64_t k = xs(&seed) % n;

			hits[i].q_offset = shuffled[k] << 9;
			hits[i].q_birth = births[k];
			misses[i].q_offset = ((xs(&seed) % SPACE_SECTORS) &
			    ~1ULL) << 9;
			misses[i].q_birth = 2 + xs(&seed) % (EPOCH - 2);
		}
		free(births);
	}

	build(shuffled, n, "random");
	for (int i = 0; i < 1000000; i++) {
		uint64_t a = now_ns();

		timer += now_ns() - a;
	}
	(void) printf("INFO timer overhead mean=%.1f ns (not subtracted)\n",
	    timer / 1e6);
	bench(hits, B_TRUE, 1);
	bench(misses, B_FALSE, 1);
	bench(hits, B_TRUE, threads);
	bench(misses, B_FALSE, threads);
	destroy();

	if (kind == 1 || kind == 2) {
		uint64_t s2 = 12345;

		/* Same entries, inserted in key order (bulk mode). */
		create();
		{
			size_t m0 = heap_used();
			uint64_t t0 = now_ns();
			ent7_t *es = malloc(n * sizeof (ent7_t));

			/* Births must match the hit queries: by sector. */
			for (uint64_t i = 0; i < n; i++)
				make_ent(shuffled[i], &s2, &es[i]);
			/* e_offset comes first: sort by it. */
			qsort(es, n, sizeof (ent7_t), u64cmp);
			m0 = heap_used();
			t0 = now_ns();
			for (uint64_t i = 0; i < n; i++)
				insert(&es[i]);
			(void) printf("MEASURE build sorted n=%llu time=%.3f s "
			    "(%.1f ns/entry) heap=%zu bytes (%.1f "
			    "bytes/entry)\n", (u_longlong_t)n,
			    (now_ns() - t0) / 1e9,
			    (double)(now_ns() - t0) / n, heap_used() - m0,
			    (double)(heap_used() - m0) / n);
			(void) printf("INFO btree height=%d nodes=%llu "
			    "leaf_cap=%u elems=%llu\n", bt.bt_height,
			    (u_longlong_t)bt.bt_num_nodes, bt.bt_leaf_cap,
			    (u_longlong_t)bt.bt_num_elems);
			free(es);
		}
		bench(hits, B_TRUE, 1);
		bench(misses, B_FALSE, 1);
		bench(hits, B_TRUE, threads);
		bench(misses, B_FALSE, threads);
		destroy();
	}
	free(hits);
	free(misses);
	free(sectors);
	free(shuffled);
	kernel_fini();
	return (0);
}
