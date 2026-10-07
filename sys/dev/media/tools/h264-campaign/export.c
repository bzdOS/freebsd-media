/* SPDX-License-Identifier: BSD-2-Clause */
/* Export exact request slices and frame-shared timestamps for trace replay. */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include "h264ctl.h"

int
main(int argc, char **argv)
{
 FILE *f; long len; uint8_t *data; size_t pos=0;
 struct h264_ctx *ctx=calloc(1,sizeof(*ctx));
 struct h264_pic *pic=calloc(1,sizeof(*pic));
 unsigned k=0,frame=0; int r;
 if(argc!=3) return 2;
 if(!(f=fopen(argv[1],"rb"))) return 1;
 if(fseek(f,0,SEEK_END)|| (len=ftell(f))<0 || fseek(f,0,SEEK_SET)) return 1;
 data=calloc(1,(size_t)len+16);
 if(!data||!ctx||!pic||fread(data,1,len,f)!=(size_t)len) return 1;
 fclose(f);
 puts("[");
 while((r=h264_next_picture(ctx,data,len,&pos,pic,(frame+1)*1000ULL))==1){
  if(k) puts(",");
  printf("{\"picture\":%u,\"frame\":%u,\"ts\":%llu,\"poc\":%d,\"field\":%d,\"bottom\":%d,\"second\":%d,\"mbaff\":%d,\"idr\":%d,\"mmco5\":%d,\"width\":%u,\"height\":%u,\"slices\":[",
   k,frame,(unsigned long long)pic->ts,pic->poc,pic->field_pic,pic->bottom_field,
   pic->second_field,pic->mbaff,pic->idr,pic->mmco5,
   (pic->sps.pic_width_in_mbs_minus1+1)*16,
   (pic->sps.pic_height_in_map_units_minus1+1)*16*((pic->sps.flags&V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY)?1:2));
  for(unsigned s=0;s<pic->nslices;s++){
   char path[1024];
   if(s) printf(",");
   if(snprintf(path,sizeof(path),"%s_%u_%u.bin",argv[2],k,s)>=(int)sizeof(path)) return 1;
   if(!(f=fopen(path,"wb"))) return 1;
   if(fwrite(pic->slice[s].data,1,pic->slice[s].len,f)!=pic->slice[s].len || fclose(f)) return 1;
   printf("{\"bytes\":%u,\"header_bits\":%u}",pic->slice[s].len,pic->slice[s].sp.header_bit_size);
  }
  printf("]}");
  if(!pic->field_pic||pic->second_field) frame++;
  k++;
 }
 puts("\n]");
 return r<0?1:0;
}
