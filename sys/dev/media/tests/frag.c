/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * frag MB — fragment physical memory: take MB megabytes of anonymous pages, touch them, give back
 * every other page and keep the rest, so the free pages are single and no contiguous run of
 * two pages survives (as far as the allocator hands out neighbours in order). Holds until killed.
 * Used by tools/pool-test.sh to reproduce F:M040-d on demand.
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	size_t mb = argc > 1 ? strtoul(argv[1], NULL, 0) : 400, ps = getpagesize(), n, i;
	char *p;

	n = (mb << 20) / ps;
	p = mmap(NULL, n * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return (1);
	}
	for (i = 0; i < n; i++)
		p[i * ps] = 1;
	for (i = 0; i < n; i += 2)
		munmap(p + i * ps, ps);
	printf("frag: kept %zu pages of %zu\n", n / 2, n);
	fflush(stdout);
	for (;;)
		pause();
}
