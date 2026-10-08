/* SPDX-License-Identifier: BSD-2-Clause */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
/* prints, per picture: structure, frame_num, then the DPB entries before it: S<fn> short, L<idx> long */
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); size_t len=ftell(f); fseek(f,0,SEEK_SET);
 uint8_t*d=malloc(len+16); fread(d,1,len,f);
 int maxp = argc>2?atoi(argv[2]):1000000;
 struct h264_ctx*c=calloc(1,sizeof*c); c->strict=0; struct h264_pic*p=calloc(1,sizeof*p); size_t pos=0; int k=0, r;
 while(k<maxp && (r=h264_next_picture(c,d,len,&pos,p,(k+1)*1000ULL))==1){
  struct v4l2_ctrl_h264_decode_params*dp=&p->dp;
  printf("%c %d %d/%d :", p->field_pic?(p->bottom_field?'B':'T'):'F', dp->frame_num, dp->top_field_order_cnt, dp->bottom_field_order_cnt);
  int idx[16],n=0; for(int i=0;i<16;i++) if(dp->dpb[i].flags&1) idx[n++]=i;
  for(int a=0;a<n;a++) for(int b=a+1;b<n;b++) { int x=idx[a],y=idx[b]; if(dp->dpb[x].frame_num>dp->dpb[y].frame_num){idx[a]=y;idx[b]=x;} }
  for(int a=0;a<n;a++){ int i=idx[a]; if(dp->dpb[i].flags&4) printf(" L%u/%d",dp->dpb[i].pic_num,dp->dpb[i].fields); else printf(" S%d/%d",dp->dpb[i].frame_num,dp->dpb[i].fields); }
  printf("\n"); k++; }
 if(r<0) printf("ERR %d\n",r);
}
