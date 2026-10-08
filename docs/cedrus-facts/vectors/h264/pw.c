#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
static struct h264_ctx ctx; static struct h264_pic pic;
int main(int argc,char**argv){FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);size_t n=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(n+8);fread(b,1,n,f);size_t pos=0;int k=0;
 while(h264_next_picture(&ctx,b,n,&pos,&pic,1000*(k+1))==1){ if(pic.slice[0].has_pw){struct v4l2_ctrl_h264_pred_weights*w=&pic.slice[0].pw;
  printf("pic %d luma_denom %u chroma_denom %u nref %u/%u\n",k,w->luma_log2_weight_denom,w->chroma_log2_weight_denom,pic.slice[0].sp.num_ref_idx_l0_active_minus1,pic.slice[0].sp.num_ref_idx_l1_active_minus1);
  for(int l=0;l<2;l++){ int nr=l?pic.slice[0].sp.num_ref_idx_l1_active_minus1:pic.slice[0].sp.num_ref_idx_l0_active_minus1; if(l&&pic.slice[0].sp.slice_type!=1)break;
   for(int i=0;i<=nr;i++) printf("  l%d[%d] luma w %d o %d  cb w %d o %d  cr w %d o %d\n",l,i,w->weight_factors[l].luma_weight[i],w->weight_factors[l].luma_offset[i],w->weight_factors[l].chroma_weight[i][0],w->weight_factors[l].chroma_offset[i][0],w->weight_factors[l].chroma_weight[i][1],w->weight_factors[l].chroma_offset[i][1]);}}
  k++;}
 return 0;}
