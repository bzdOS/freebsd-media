#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/videodev2.h>
#include HDR
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); size_t len=ftell(f); fseek(f,0,SEEK_SET);
 uint8_t*d=malloc(len+16); fread(d,1,len,f);
 struct h264_ctx*c=calloc(1,sizeof*c); struct h264_pic*p=calloc(1,sizeof*p); size_t pos=0; int k=0, frame=0, r;
 while((r=h264_next_picture(c,d,len,&pos,p,(frame+1)*1000ULL))==1){
  struct v4l2_ctrl_h264_decode_params*dp=&p->dp;
  printf("pic %d ts %llu poc %d idr %d ref %d ns %u dp: nal %d fn %d poc %d/%d idr_id %d lsb %d db %d d0 %d d1 %d mb %u pb %u fl %x\n",k,(unsigned long long)p->ts,p->poc,p->idr,p->is_ref,p->nslices,dp->nal_ref_idc,dp->frame_num,dp->top_field_order_cnt,dp->bottom_field_order_cnt,dp->idr_pic_id,dp->pic_order_cnt_lsb,dp->delta_pic_order_cnt_bottom,dp->delta_pic_order_cnt0,dp->delta_pic_order_cnt1,dp->dec_ref_pic_marking_bit_size,dp->pic_order_cnt_bit_size,dp->flags);
  for(int i=0;i<16;i++) if(dp->dpb[i].flags&1) printf("  dpb %d ts %llu fn %d pn %d poc %d/%d fields %d fl %x\n",i,(unsigned long long)dp->dpb[i].reference_ts,dp->dpb[i].frame_num,dp->dpb[i].pic_num,dp->dpb[i].top_field_order_cnt,dp->dpb[i].bottom_field_order_cnt,dp->dpb[i].fields,dp->dpb[i].flags);
  for(unsigned sl=0;sl<p->nslices;sl++){ struct v4l2_ctrl_h264_slice_params*s=&p->slice[sl].sp;
   printf("  sl %u type %d mb %u hdr %u nref %d/%d qp %d dbf %d %d %d cabac %d fl %x l0:",sl,s->slice_type,s->first_mb_in_slice,s->header_bit_size,s->num_ref_idx_l0_active_minus1,s->num_ref_idx_l1_active_minus1,s->slice_qp_delta,s->disable_deblocking_filter_idc,s->slice_alpha_c0_offset_div2,s->slice_beta_offset_div2,s->cabac_init_idc,s->flags);
   for(int i=0;i<32;i++) printf(" %d.%d",s->ref_pic_list0[i].index,s->ref_pic_list0[i].fields); printf(" l1:");
   for(int i=0;i<32;i++) printf(" %d.%d",s->ref_pic_list1[i].index,s->ref_pic_list1[i].fields); printf("\n"); }
  if (!p->field_pic || p->second_field) frame++;
  k++; }
 printf("end %d %d\n",k,r);
}
