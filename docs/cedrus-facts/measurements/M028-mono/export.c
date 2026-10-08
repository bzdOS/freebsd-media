/* SPDX-License-Identifier: BSD-2-Clause */
/* Observer/exporter over our BSD front end; no decoder implementation here. */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include "h264ctl.h"
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 1;
 fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
 uint8_t *data=calloc(1,len+16);if(!data||fread(data,1,len,f)!=(size_t)len)return 1;fclose(f);
 struct h264_ctx *c=calloc(1,sizeof(*c));struct h264_pic *p=calloc(1,sizeof(*p));size_t pos=0;
 if(!c||!p||h264_next_picture(c,data,len,&pos,p,1000)!=1)return 1;
 if(p->nslices!=1||p->slice[0].sp.slice_type!=2||p->field_pic)return 3;
 const struct v4l2_ctrl_h264_sps *s=&p->sps;const struct v4l2_ctrl_h264_pps *q=&p->pps;
 const struct v4l2_ctrl_h264_slice_params *sl=&p->slice[0].sp;
 f=fopen(argv[2],"wb");if(!f||fwrite(p->slice[0].data,1,p->slice[0].len,f)!=p->slice[0].len)return 1;fclose(f);
 char smpath[1024];if(snprintf(smpath,sizeof(smpath),"%s.scaling",argv[2])>=(int)sizeof(smpath))return 1;
 f=fopen(smpath,"wb");if(!f||fwrite(&p->sm,1,sizeof(p->sm),f)!=sizeof(p->sm))return 1;fclose(f);
 unsigned flags=((q->flags&V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE)?1u<<15:0)|((q->flags&V4L2_H264_PPS_FLAG_WEIGHTED_PRED)?1u<<4:0)|(q->weighted_bipred_idc<<2)|((q->flags&V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED)?2:0)|((q->flags&V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE)?1:0);
 unsigned qp=((q->flags&V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)?0:1u<<24)|((q->second_chroma_qp_index_offset&63)<<16)|((q->chroma_qp_index_offset&63)<<8)|(26+q->pic_init_qp_minus26+sl->slice_qp_delta);
 printf("{\"wmb\":%u,\"hmb\":%u,\"sps_flags\":%u,\"chroma\":%u,\"depth\":%u,\"pps_flags\":%u,\"flags_reg\":%u,\"qp_reg\":%u,\"slice_reg\":%u,\"dbf_reg\":%u,\"header_bits\":%u,\"bytes\":%u,\"scaling\":%u}\n",s->pic_width_in_mbs_minus1+1,s->pic_height_in_map_units_minus1+1,s->flags,s->chroma_format_idc,s->bit_depth_luma_minus8,q->flags,flags,qp,(p->dp.nal_ref_idc?1u<<12:0)|(2u<<8)|(sl->cabac_init_idc&3),0x1000|(sl->disable_deblocking_filter_idc<<8)|((sl->slice_alpha_c0_offset_div2&15)<<4)|(sl->slice_beta_offset_div2&15),sl->header_bit_size,p->slice[0].len,!!(q->flags&V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT));
 return 0;
}
