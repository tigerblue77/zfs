// SPDX-License-Identifier: CDDL-1.0
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * https://opensource.org/license/CDDL-1.0.
 */

#include <sys/zfs_context.h>
#include <sys/vdev_indirect_mapping.h>

#include "unit.h"

/* ========== */

#define	MAX_ENTRIES	8
#define	SECTOR		(1ULL << SPA_MINBLOCKSHIFT)

/*
 * A mapping held in memory only.  The lookup only reads the entries and
 * vimp_num_entries; the other fields just have to satisfy
 * vdev_indirect_mapping_verify().
 */
typedef struct {
	vdev_indirect_mapping_t		m_vim;
	vdev_indirect_mapping_phys_t	m_phys;
	vdev_indirect_mapping_entry_phys_t m_entries[MAX_ENTRIES];
} mapping_t;

/*
 * Entry i covers sectors [3i + 1, 3i + 3), so there is a gap of one sector
 * before every entry, including the first.
 */
static void
mapping_init(mapping_t *m, uint64_t n)
{
	memset(m, 0, sizeof (*m));
	for (uint64_t i = 0; i < n; i++) {
		DVA_MAPPING_SET_SRC_OFFSET(&m->m_entries[i],
		    (3 * i + 1) * SECTOR);
		DVA_SET_ASIZE(&m->m_entries[i].vimep_dst, 2 * SECTOR);
	}
	m->m_phys.vimp_num_entries = n;
	m->m_phys.vimp_max_offset = (3 * n) * SECTOR;
	m->m_vim.vim_object = 1;
	m->m_vim.vim_objset = (objset_t *)m;
	m->m_vim.vim_dbuf = (dmu_buf_t *)m;
	m->m_vim.vim_phys = &m->m_phys;
	m->m_vim.vim_entries = m->m_entries;
}

/* The entry containing the offset, or else the first one after it. */
static vdev_indirect_mapping_entry_phys_t *
mapping_find_linear(mapping_t *m, uint64_t offset, boolean_t next)
{
	for (uint64_t i = 0; i < m->m_phys.vimp_num_entries; i++) {
		vdev_indirect_mapping_entry_phys_t *e = &m->m_entries[i];
		uint64_t start = DVA_MAPPING_GET_SRC_OFFSET(e);

		if (offset < start)
			return (next ? e : NULL);
		if (offset < start + DVA_GET_ASIZE(&e->vimep_dst))
			return (e);
	}
	return (NULL);
}

/*
 * Compare both lookups with a linear search on every sector, from below
 * the first entry to past the last one, for each table size.
 */
static MunitResult
test_indirect_mapping_lookup(const MunitParameter params[], void *data)
{
	(void) params, (void) data;

	mapping_t *m = malloc(sizeof (*m));
	unit_notnull(m);

	for (uint64_t n = 1; n <= MAX_ENTRIES; n++) {
		mapping_init(m, n);
		for (uint64_t s = 0; s <= 3 * n + 1; s++) {
			uint64_t offset = s * SECTOR;

			munit_assert_ptr_equal(
			    vdev_indirect_mapping_entry_for_offset(&m->m_vim,
			    offset), mapping_find_linear(m, offset, B_FALSE));
			munit_assert_ptr_equal(
			    vdev_indirect_mapping_entry_for_offset_or_next(
			    &m->m_vim, offset),
			    mapping_find_linear(m, offset, B_TRUE));
		}
	}

	free(m);
	return (MUNIT_OK);
}

/* ========== */

static const MunitTest indirect_mapping_tests[] = {
	UNIT_TEST("lookup",	test_indirect_mapping_lookup),
	{ 0 },
};

static const MunitSuite indirect_mapping_test_suite = {
	"indirect_mapping.",
	indirect_mapping_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE,
};

int
main(int argc, char **argv)
{
	return (munit_suite_main(&indirect_mapping_test_suite, NULL, argc,
	    argv));
}
