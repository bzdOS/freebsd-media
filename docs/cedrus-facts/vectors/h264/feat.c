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
	while (k < 8 && h264_next_picture(&ctx, b, n, &pos, &pic, 1000*(k+1)) == 1) {
		for (unsigned s=0;s<pic.nslices;s++) {
			struct v4l2_ctrl_h264_slice_params *sp=&pic.slice[s].sp;
			int nv=0; for (int i=0;i<16;i++) if (pic.dp.dpb[i].flags&1) nv++;
			printf("profile=%u level=%u log2fn=%u poctype=%u log2poc=%u maxref=%u spsflags=%x wmb=%u hmb=%u "
			  "ppsflags=%x wbi=%u initqp=%d cqp=%d/%d nref_def=%u/%u "
			  "stype=%u firstmb=%u qpd=%d dbf=%u alpha=%d beta=%d cabacidc=%u nref=%u/%u direct=%u "
			  "nalref=%u fnum=%u idr=%u poc=%d/%d idrid=%u dpflags=%x markbits=%u pocbits=%u ndpb=%d hdr=%u len=%u nsl=%u sl=%u\n",
			  pic.sps.profile_idc,pic.sps.level_idc,pic.sps.log2_max_frame_num_minus4+4,pic.sps.pic_order_cnt_type,pic.sps.log2_max_pic_order_cnt_lsb_minus4+4,pic.sps.max_num_ref_frames,pic.sps.flags,
			  pic.sps.pic_width_in_mbs_minus1+1,pic.sps.pic_height_in_map_units_minus1+1,
			  pic.pps.flags,pic.pps.weighted_bipred_idc,pic.pps.pic_init_qp_minus26+26,pic.pps.chroma_qp_index_offset,pic.pps.second_chroma_qp_index_offset,pic.pps.num_ref_idx_l0_default_active_minus1,pic.pps.num_ref_idx_l1_default_active_minus1,
			  sp->slice_type,sp->first_mb_in_slice,sp->slice_qp_delta,sp->disable_deblocking_filter_idc,sp->slice_alpha_c0_offset_div2,sp->slice_beta_offset_div2,sp->cabac_init_idc,sp->num_ref_idx_l0_active_minus1,sp->num_ref_idx_l1_active_minus1,(sp->flags&1),
			  pic.dp.nal_ref_idc,pic.dp.frame_num,pic.idr,pic.dp.top_field_order_cnt,pic.dp.bottom_field_order_cnt,pic.dp.idr_pic_id,pic.dp.flags,pic.dp.dec_ref_pic_marking_bit_size,pic.dp.pic_order_cnt_bit_size,nv,sp->header_bit_size,pic.slice[s].len,pic.nslices,s);
		}
		k++;
	}
	return 0;
}
