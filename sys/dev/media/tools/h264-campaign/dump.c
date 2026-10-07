#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); size_t len=ftell(f); fseek(f,0,SEEK_SET);
 uint8_t*d=malloc(len+16); fread(d,1,len,f);
 int maxp = argc>2?atoi(argv[2]):1000000, verbose = argc>3;
 struct h264_ctx*c=calloc(1,sizeof*c); c->strict=0; struct h264_pic*p=calloc(1,sizeof*p); size_t pos=0; int k=0, r;
 while(k<maxp && (r=h264_next_picture(c,d,len,&pos,p,(k+1)*1000ULL))==1){
  struct v4l2_ctrl_h264_slice_params*s=&p->slice[0].sp; struct v4l2_ctrl_h264_decode_params*dp=&p->dp;
  printf("pic %d %s%s%s type %d fn %d poc %d/%d ref %d%s nsl %d nref %d/%d\n",k,p->field_pic?(p->bottom_field?"B-field":"T-field"):"frame",p->second_field?"(2nd)":"",p->mbaff?"(mbaff)":"",s->slice_type,dp->frame_num,dp->top_field_order_cnt,dp->bottom_field_order_cnt,p->is_ref,p->mmco5?" MMCO5":"",p->nslices,s->num_ref_idx_l0_active_minus1+1,s->num_ref_idx_l1_active_minus1+1);
  if(verbose){
   for(int i=0;i<16;i++) if(dp->dpb[i].flags&1) printf("   dpb[%d] ts %llu fn %d pn %d poc %d/%d fields %d fl %x\n",i,(unsigned long long)dp->dpb[i].reference_ts,dp->dpb[i].frame_num,dp->dpb[i].pic_num,dp->dpb[i].top_field_order_cnt,dp->dpb[i].bottom_field_order_cnt,dp->dpb[i].fields,dp->dpb[i].flags);
   for(int sl=0;sl<(int)p->nslices && sl<1;sl++){ s=&p->slice[sl].sp;
    printf("   l0:"); for(int i=0;i<s->num_ref_idx_l0_active_minus1+1&&s->slice_type!=2;i++) printf(" %d.%d",s->ref_pic_list0[i].index,s->ref_pic_list0[i].fields);
    printf("  l1:"); for(int i=0;i<s->num_ref_idx_l1_active_minus1+1&&s->slice_type==1;i++) printf(" %d.%d",s->ref_pic_list1[i].index,s->ref_pic_list1[i].fields); printf("\n"); }
   printf("   dpb after:"); for(int i=0;i<16;i++) if(c->dpb[i].used) printf(" [%d fn%d m%d%d idx%d poc%d/%d]",i,c->dpb[i].frame_num,c->dpb[i].mark[0],c->dpb[i].mark[1],c->dpb[i].long_idx,c->dpb[i].poc_f[0],c->dpb[i].poc_f[1]); printf("\n");
  }
  k++; }
 if(r<0) printf("stopped: %d\n",r);
 printf("%s: %d pictures\n",argv[1],k);
}
