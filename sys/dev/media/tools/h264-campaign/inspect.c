/* SPDX-License-Identifier: BSD-2-Clause */
/* Inventory parser coverage over a complete Annex B stream; no decode claim. */
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include "h264ctl.h"

int
main(int argc, char **argv)
{
	struct h264_ctx *ctx;
	struct h264_pic *pic;
	FILE *fp;
	uint8_t *data;
	long length;
	size_t pos = 0;
	unsigned pictures = 0, frames = 0, slices = 0, fields = 0, mbaff = 0;
	unsigned long_term = 0, explicit_b = 0, i;
	int result, pending_field = 0;

	if (argc != 2 || (fp = fopen(argv[1], "rb")) == NULL)
		return (2);
	if (fseek(fp, 0, SEEK_END) || (length = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET))
		return (2);
	data = calloc(1, (size_t)length + 16);
	ctx = calloc(1, sizeof(*ctx));
	pic = calloc(1, sizeof(*pic));
	if (data == NULL || ctx == NULL || pic == NULL ||
	    fread(data, 1, length, fp) != (size_t)length)
		return (2);
	fclose(fp);
	ctx->strict = 1;
	while ((result = h264_next_picture(ctx, data, length, &pos, pic,
	    (uint64_t)(frames + 1) * 1000)) == 1) {
		int has_long = 0;

		pictures++;
		slices += pic->nslices;
		fields += pic->field_pic != 0;
		mbaff += pic->mbaff != 0;
		pending_field = pic->field_pic && !pic->second_field;
		frames += !pending_field;
		for (i = 0; i < 16; i++)
			if (pic->dp.dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
				has_long = 1;
		long_term += has_long;
		for (i = 0; i < pic->nslices; i++)
			if (pic->slice[i].sp.slice_type == V4L2_H264_SLICE_TYPE_B &&
			    pic->pps.weighted_bipred_idc == 1)
				explicit_b++;
	}
	if (result == 0 && pending_field)
		result = -13;
	printf("{\"parse_result\":%d,\"pictures\":%u,\"frames\":%u,\"slices\":%u,"
	    "\"field_pictures\":%u,\"mbaff_pictures\":%u,\"long_term_pictures\":%u,"
	    "\"explicit_b_slices\":%u}\n", result, pictures, frames, slices,
	    fields, mbaff, long_term, explicit_b);
	free(pic);
	free(ctx);
	free(data);
	return (0);
}
