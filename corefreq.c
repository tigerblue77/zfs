/* Core clock / TSC ratio from a chain of dependent imul (3-cycle latency). */
#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>
int main(void) {
	for (int r = 0; r < 5; r++) {
		uint64_t x = 3, y = 5, n = 20000000;
		uint64_t t0 = __rdtsc();
		for (uint64_t i = 0; i < n; i++)
			__asm__ volatile(
			    "imul %1,%0\n\timul %1,%0\n\timul %1,%0\n\timul %1,%0\n\t"
			    "imul %1,%0\n\timul %1,%0\n\timul %1,%0\n\timul %1,%0"
			    : "+r"(x) : "r"(y));
		uint64_t t1 = __rdtsc();
		double ratio = 3.0 * 8.0 * n / (t1 - t0);
		printf("core/TSC = %.3f  (core ~ %.2f GHz at 2.10 GHz TSC)\n",
		    ratio, 2.1 * ratio);
	}
	return 0;
}
