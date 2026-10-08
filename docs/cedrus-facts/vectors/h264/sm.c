#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
static struct h264_ctx ctx; static struct h264_pic pic;
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);size_t n=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(n+8);fread(b,1,n,f);size_t pos=0;
 if(h264_next_picture(&ctx,b,n,&pos,&pic,1000)==1){printf("has_sm %d\n",pic.has_sm);
 for(int i=0;i<6;i++){printf("4x4[%d]:",i);for(int j=0;j<16;j++)printf(" %d",pic.sm.scaling_list_4x4[i][j]);printf("\n");}
 for(int i=0;i<2;i++){printf("8x8[%d]:",i);for(int j=0;j<64;j++)printf(" %d",pic.sm.scaling_list_8x8[i][j]);printf("\n");}}
 return 0;}
