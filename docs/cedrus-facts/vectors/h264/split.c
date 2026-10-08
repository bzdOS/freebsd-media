#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
static struct h264_ctx ctx; static struct h264_pic pic;
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb"); const char *prefix = argv[2];
	fseek(f,0,SEEK_END); size_t n=ftell(f); fseek(f,0,SEEK_SET);
	uint8_t *b = malloc(n+8); fread(b,1,n,f); size_t pos=0; int k=0;
	while (h264_next_picture(&ctx, b, n, &pos, &pic, 1000*(k+1)) == 1) {
		for (unsigned s=0;s<pic.nslices;s++) {
			char fn[256]; snprintf(fn,sizeof fn,"%s_%d_%u.bin",prefix,k,s);
			FILE *o=fopen(fn,"wb"); fwrite(pic.slice[s].data,1,pic.slice[s].len,o); fclose(o);
		}
		k++;
	}
	return 0;
}
