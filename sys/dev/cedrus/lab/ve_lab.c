/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ve_lab: script interpreter over /dev/cedrus-lab, for finding out how the
 * video engine is programmed. Commands (one per line, '#' comments, numbers in
 * C notation, "pa" is the physical address of the lab buffer):
 *
 *   rd <off>                    print register
 *   wr <off> <val|expr>         write register; expr: pa+0x1000, pa+0x1000>>4 ...
 *   wrpa <off> <bufoff>         write the address of buffer+bufoff relative to the DRAM base (M003)
 *   wrvld <off> <bufoff> <flag> write a VLD-style address: flag<<28 | pa[27:4] | pa[31:28]
 *   load <file> <bufoff>        copy a file into the buffer
 *   fill <bufoff> <len> <byte>
 *   nz <bufoff> <len>           count non-zero bytes, first and last offset
 *   dump <bufoff> <len> <file>  save buffer bytes
 *   hex <bufoff> <len>          print words
 *   poll <off> <mask> <val> <ms> wait until (reg & mask) == val
 *   reset                       module reset
 *   sleep <ms>
 *   echo <text>
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../cedrus_lab.h"

static int fd;
static uint8_t *buf;
static struct cedrus_lab_info info;

static uint64_t
val(const char *s)
{
	uint64_t v = 0, a;
	char op = '+';
	char *e;

	for (;;) {
		if (strncmp(s, "pa", 2) == 0) {
			a = info.buf_pa;
			s += 2;
		} else {
			a = strtoull(s, &e, 0);
			s = e;
		}
		switch (op) {
		case '+': v += a; break;
		case '-': v -= a; break;
		case '|': v |= a; break;
		case '&': v &= a; break;
		case '<': v <<= a; break;
		case '>': v >>= a; break;
		}
		if (*s == '\0')
			return (v);
		op = *s++;
		if ((op == '<' || op == '>') && *s == op)
			s++;
	}
}

static uint32_t
rd(uint32_t off)
{
	struct cedrus_lab_reg r = { off, 0 };

	if (ioctl(fd, CEDRUS_LAB_RD, &r) != 0)
		err(1, "rd %#x", off);
	return (r.val);
}

static void
wr(uint32_t off, uint32_t v)
{
	struct cedrus_lab_reg r = { off, v };

	if (ioctl(fd, CEDRUS_LAB_WR, &r) != 0)
		err(1, "wr %#x", off);
}

int
main(int argc, char **argv)
{
	char line[512], *t[8], *p;
	FILE *in = argc > 1 ? fopen(argv[1], "r") : stdin;
	int n;

	if (in == NULL)
		err(1, "%s", argv[1]);
	fd = open("/dev/cedrus-lab", O_RDWR);
	if (fd < 0)
		err(1, "/dev/cedrus-lab");
	if (ioctl(fd, CEDRUS_LAB_INFO, &info) != 0)
		err(1, "info");
	buf = mmap(NULL, info.buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED)
		err(1, "mmap");
	printf("buffer pa=%#jx size=%ju\n", (uintmax_t)info.buf_pa, (uintmax_t)info.buf_size);

	while (fgets(line, sizeof(line), in) != NULL) {
		if ((p = strchr(line, '#')) != NULL)
			*p = '\0';
		for (n = 0, p = strtok(line, " \t\r\n"); p != NULL && n < 8;
		    p = strtok(NULL, " \t\r\n"))
			t[n++] = p;
		if (n == 0)
			continue;
		if (strcmp(t[0], "rd") == 0 && n == 2) {
			printf("%03jx = %08x\n", (uintmax_t)val(t[1]), rd(val(t[1])));
		} else if (strcmp(t[0], "wr") == 0 && n == 3) {
			wr(val(t[1]), val(t[2]));
		} else if (strcmp(t[0], "wrpa") == 0 && n == 3) {
			wr(val(t[1]), (uint32_t)(info.buf_pa + val(t[2]) - info.dma_base));
		} else if (strcmp(t[0], "wrvld") == 0 && n == 4) {
			uint64_t a = info.buf_pa + val(t[2]) - info.dma_base;

			wr(val(t[1]), (uint32_t)((val(t[3]) << 28) | (a & 0x0ffffff0) |
			    ((a >> 28) & 0xf)));
		} else if (strcmp(t[0], "load") == 0 && n == 3) {
			FILE *f = fopen(t[1], "rb");
			size_t got;

			if (f == NULL)
				err(1, "%s", t[1]);
			got = fread(buf + val(t[2]), 1, info.buf_size - val(t[2]), f);
			fclose(f);
			printf("loaded %zu bytes at +%#jx\n", got, (uintmax_t)val(t[2]));
		} else if (strcmp(t[0], "fill") == 0 && n == 4) {
			memset(buf + val(t[1]), (int)val(t[3]), val(t[2]));
		} else if (strcmp(t[0], "nz") == 0 && n == 3) {
			uint64_t o = val(t[1]), l = val(t[2]), i, c = 0, first = 0, last = 0;

			for (i = 0; i < l; i++)
				if (buf[o + i] != 0) {
					if (c++ == 0)
						first = o + i;
					last = o + i;
				}
			printf("nz +%#jx len %#jx: %ju non-zero, first +%#jx last +%#jx\n",
			    (uintmax_t)o, (uintmax_t)l, (uintmax_t)c, (uintmax_t)first,
			    (uintmax_t)last);
		} else if (strcmp(t[0], "dump") == 0 && n == 4) {
			FILE *f = fopen(t[3], "wb");

			if (f == NULL)
				err(1, "%s", t[3]);
			fwrite(buf + val(t[1]), 1, val(t[2]), f);
			fclose(f);
		} else if (strcmp(t[0], "hex") == 0 && n == 3) {
			uint64_t o = val(t[1]), l = val(t[2]), i;

			for (i = 0; i < l; i += 16) {
				uint32_t *w = (uint32_t *)(buf + o + i);

				printf("%06jx: %08x %08x %08x %08x\n", (uintmax_t)(o + i),
				    w[0], w[1], w[2], w[3]);
			}
		} else if (strcmp(t[0], "poll") == 0 && n == 5) {
			uint32_t off = val(t[1]), mask = val(t[2]), want = val(t[3]);
			int ms = val(t[4]), i, ok = 0;

			for (i = 0; i < ms; i++) {
				if ((rd(off) & mask) == want) {
					ok = 1;
					break;
				}
				usleep(1000);
			}
			printf("poll %03x &%x ==%x: %s after %d ms (now %08x)\n", off,
			    mask, want, ok ? "ok" : "TIMEOUT", i, rd(off));
		} else if (strcmp(t[0], "reset") == 0) {
			ioctl(fd, CEDRUS_LAB_ENGINE_RESET);
		} else if (strcmp(t[0], "sleep") == 0 && n == 2) {
			usleep(val(t[1]) * 1000);
		} else if (strcmp(t[0], "echo") == 0) {
			for (int i = 1; i < n; i++)
				printf("%s ", t[i]);
			printf("\n");
		} else {
			errx(1, "bad command: %s", t[0]);
		}
		fflush(stdout);
	}
	return (0);
}
