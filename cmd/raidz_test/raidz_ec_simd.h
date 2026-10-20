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

#ifndef	RAIDZ_EC_SIMD_H
#define	RAIDZ_EC_SIMD_H

#include <stdint.h>
#include <stddef.h>

/* Most output columns of one dot product. */
#define	RZEC_SIMD_MAXROWS	6

/*
 * dst[d] = src[0] ^ ... ^ src[ns - 1] for d < nx (nx is 0 or 1), and
 * dst[nx + d] = sum_j c[d][j] * src[j] for d < nm, over len bytes.  tab
 * holds the table of each c[d][j] at offset (j * nm + d) * rs_tabsize,
 * and len is a multiple of rs_step.
 */
typedef void (*rzec_dot_f)(uint8_t *const *dst, int nx, int nm,
    const uint8_t *const *src, int ns, const uint8_t *tab, size_t len);

typedef struct rzec_simd {
	const char	*rs_name;
	int		(*rs_available)(void);
	const uint8_t	*rs_tab;	/* table of c at c * rs_tabsize */
	size_t		rs_tabsize;
	size_t		rs_step;
	rzec_dot_f	rs_dot;
} rzec_simd_t;

extern const rzec_simd_t *const rzec_simd_impls[];
extern const int rzec_simd_nimpls;

void rzec_simd_init(const uint8_t mul[256][256]);

#endif /* RAIDZ_EC_SIMD_H */
