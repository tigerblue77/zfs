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

/*
 * One SIMD back-end of the dot product, included by raidz_ec_simd.c once
 * per instruction set, with these defined:
 *
 * RZ_PFX			prefix of the function names
 * RZ_TGT			target attribute of every function
 * RZ_V, RZ_W			vector type and its width in bytes
 * RZ_U				vectors per output per iteration
 * RZ_TS			table bytes per coefficient
 * RZ_LOAD(p), RZ_STORE(p, v), RZ_ZERO(), RZ_XOR(a, b)
 * RZ_PREP(s, lo, hi)		per-source operands shared by every RZ_MAC
 * RZ_MAC(acc, t, s, lo, hi)	acc ^= c * s, with the table of c at t
 *
 * Every output stays in registers while all the sources are read once,
 * so a row is read once and written once.  nx and nm are constants in
 * each instance, so that the loops over the outputs unroll.
 */

#define	RZ_CAT2(a, b)	a##_##b
#define	RZ_CAT(a, b)	RZ_CAT2(a, b)
#define	RZ_FN(x)	RZ_CAT(RZ_PFX, x)

static inline __attribute__((always_inline)) RZ_TGT void
RZ_FN(dot_body)(uint8_t *const *dst, const int nx, const int nm,
    const uint8_t *const *src, int ns, const uint8_t *tab, size_t len)
{
	const int nd = nx + nm;

	for (size_t i = 0; i < len; i += RZ_W * RZ_U) {
		RZ_V acc[RZEC_SIMD_MAXROWS][RZ_U];
		const uint8_t *t = tab;

#pragma GCC unroll 8
		for (int d = 0; d < nd; d++)
#pragma GCC unroll 4
			for (int u = 0; u < RZ_U; u++)
				acc[d][u] = RZ_ZERO();

		for (int j = 0; j < ns; j++) {
			RZ_V s[RZ_U], lo[RZ_U], hi[RZ_U];

#pragma GCC unroll 4
			for (int u = 0; u < RZ_U; u++) {
				s[u] = RZ_LOAD(src[j] + i + u * RZ_W);
				RZ_PREP(s[u], lo[u], hi[u]);
			}
			if (nx != 0) {
#pragma GCC unroll 4
				for (int u = 0; u < RZ_U; u++)
					acc[0][u] = RZ_XOR(acc[0][u], s[u]);
			}
#pragma GCC unroll 8
			for (int d = 0; d < nm; d++) {
#pragma GCC unroll 4
				for (int u = 0; u < RZ_U; u++)
					RZ_MAC(acc[nx + d][u], t + d * RZ_TS,
					    s[u], lo[u], hi[u]);
			}
			t += nm * RZ_TS;
		}

#pragma GCC unroll 8
		for (int d = 0; d < nd; d++)
#pragma GCC unroll 4
			for (int u = 0; u < RZ_U; u++)
				RZ_STORE(dst[d] + i + u * RZ_W, acc[d][u]);
	}
}

#define	RZ_INST(NX, NM)							\
static RZ_TGT void							\
RZ_FN(dot_##NX##_##NM)(uint8_t *const *dst, int nx, int nm,		\
    const uint8_t *const *src, int ns, const uint8_t *tab, size_t len)	\
{									\
	(void) nx, (void) nm;						\
	RZ_FN(dot_body)(dst, NX, NM, src, ns, tab, len);		\
}

RZ_INST(0, 1)
RZ_INST(0, 2)
RZ_INST(0, 3)
RZ_INST(0, 4)
RZ_INST(0, 5)
RZ_INST(0, 6)
RZ_INST(1, 0)
RZ_INST(1, 1)
RZ_INST(1, 2)
RZ_INST(1, 3)
RZ_INST(1, 4)
RZ_INST(1, 5)

static const rzec_dot_f RZ_FN(dots)[2][RZEC_SIMD_MAXROWS + 1] = {
	{ NULL, RZ_FN(dot_0_1), RZ_FN(dot_0_2), RZ_FN(dot_0_3),
	    RZ_FN(dot_0_4), RZ_FN(dot_0_5), RZ_FN(dot_0_6) },
	{ RZ_FN(dot_1_0), RZ_FN(dot_1_1), RZ_FN(dot_1_2), RZ_FN(dot_1_3),
	    RZ_FN(dot_1_4), RZ_FN(dot_1_5), NULL },
};

static void
RZ_FN(dot)(uint8_t *const *dst, int nx, int nm, const uint8_t *const *src,
    int ns, const uint8_t *tab, size_t len)
{
	rzec_dot_f f = RZ_FN(dots)[nx][nm];

	if (f != NULL)
		f(dst, nx, nm, src, ns, tab, len);
}

#undef	RZ_INST
#undef	RZ_FN
#undef	RZ_CAT
#undef	RZ_CAT2
#undef	RZ_PFX
#undef	RZ_TGT
#undef	RZ_V
#undef	RZ_W
#undef	RZ_U
#undef	RZ_TS
#undef	RZ_LOAD
#undef	RZ_STORE
#undef	RZ_ZERO
#undef	RZ_XOR
#undef	RZ_PREP
#undef	RZ_MAC
