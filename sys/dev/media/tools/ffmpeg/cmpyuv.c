/* SPDX-License-Identifier: BSD-2-Clause */
/* cmpyuv a.yuv b.yuv [tolerance]: sample-wise comparison of two raw files; prints the largest difference and
 * how many samples differ by more than the tolerance (default 0). Exit 0 when none does. */
#include <stdio.h>
#include <stdlib.h>
int
main(int argc, char **argv)
{
	FILE *a, *b;
	int ca, cb, tol = argc > 3 ? atoi(argv[3]) : 0, max = 0;
	unsigned long n = 0, bad = 0, nd = 0;

	if (argc < 3 || (a = fopen(argv[1], "rb")) == NULL || (b = fopen(argv[2], "rb")) == NULL)
		return (2);
	while ((ca = getc(a)) != EOF && (cb = getc(b)) != EOF) {
		int d = abs(ca - cb);

		n++;
		if (d != 0)
			nd++;
		if (d > max)
			max = d;
		if (d > tol)
			bad++;
	}
	if (getc(a) != EOF || getc(b) != EOF) {
		printf("length differs\n");
		return (1);
	}
	printf("samples %lu, differing %lu, max diff %d, beyond tolerance %d: %lu\n", n, nd, max, tol, bad);
	return (bad != 0);
}
