/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
static struct h264_ctx ctx; static struct h264_pic pic;
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); fseek(f,0,SEEK_END); size_t n=ftell(f); fseek(f,0,SEEK_SET);
	uint8_t *b = malloc(n+8); fread(b,1,n,f); size_t pos=0; int k=0;
	while (h264_next_picture(&ctx, b, n, &pos, &pic, 1000*(k+1)) == 1) { printf("%d %d\n", pic.idr, pic.poc); k++; }
	return 0;
}
