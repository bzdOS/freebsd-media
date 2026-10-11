/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_h264_reject: does the decoder refuse, at the moment the controls are set,
 * a stream it cannot decode?
 *
 *   cedrus_h264_reject [-a] [-i what] [-v video] [-m media] stream.264
 *
 * The stream is parsed without the engine's limits (the test's parser otherwise
 * stops at what the engine cannot do) up to the first picture that shows the
 * feature: for an SP stream the first SP slice, otherwise the first picture.
 * Its SPS, PPS and slice parameters, with the scaling matrix, weights and decode
 * parameters, then go to the driver three ways: TRY_EXT_CTRLS, S_EXT_CTRLS on
 * the current values, and S_EXT_CTRLS into a request. Each must fail with
 * EINVAL, and the current SPS must be what it was before. Nothing is queued.
 *
 * -i injects one unsupported value into a supported stream's controls instead
 * (fmo, sp, si, plane, plane_id, 422, 444, 10bit, 14bit); -a expects every call
 * to succeed (the positive control, on a supported stream).
 *
 *   cc -I<shim> -I<dir with linux/videodev2.h> -o cedrus_h264_reject cedrus_h264_reject.c
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <linux/media.h>

#include "h264ctl.h"

static struct v4l2_ctrl_h264_scaling_matrix flat_sm;
static struct v4l2_ctrl_h264_pred_weights no_pw;

static void
die(const char *what)
{

	perror(what);
	exit(2);
}

static void
inject(struct h264_pic *pic, const char *what)
{

	if (strcmp(what, "fmo") == 0)
		pic->pps.num_slice_groups_minus1 = 1;
	else if (strcmp(what, "sp") == 0)
		pic->slice[0].sp.slice_type = V4L2_H264_SLICE_TYPE_SP;
	else if (strcmp(what, "si") == 0)
		pic->slice[0].sp.slice_type = V4L2_H264_SLICE_TYPE_SI;
	else if (strcmp(what, "plane") == 0)
		pic->sps.flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
	else if (strcmp(what, "plane_id") == 0)
		pic->slice[0].sp.colour_plane_id = 1;
	else if (strcmp(what, "422") == 0)
		pic->sps.chroma_format_idc = 2;
	else if (strcmp(what, "444") == 0)
		pic->sps.chroma_format_idc = 3;
	else if (strcmp(what, "10bit") == 0)
		pic->sps.bit_depth_luma_minus8 = pic->sps.bit_depth_chroma_minus8 = 2;
	else if (strcmp(what, "14bit") == 0)
		pic->sps.bit_depth_luma_minus8 = pic->sps.bit_depth_chroma_minus8 = 6;
	else {
		fprintf(stderr, "unknown injection %s\n", what);
		exit(2);
	}
}

/* One call; returns 0 when its outcome is the expected one. */
static int
call(int fd, unsigned long req, struct v4l2_ext_controls *ecs, const char *name, int accept)
{
	int r = ioctl(fd, req, ecs), e = errno;

	printf("  %-22s %s (error_idx %u of %u)\n", name,
	    r == 0 ? "accepted" : strerror(e), ecs->error_idx, ecs->count);
	if (accept)
		return (r == 0 ? 0 : 1);
	return (r != 0 && e == EINVAL ? 0 : 1);
}

int
main(int argc, char **argv)
{
	const char *vpath = "/dev/video0", *mpath = "/dev/media0", *what = NULL;
	struct h264_ctx *hc = calloc(1, sizeof(*hc));
	struct h264_pic *pic = calloc(1, sizeof(*pic));
	struct v4l2_ctrl_h264_sps before, after;
	struct v4l2_ext_control ec[6], one;
	struct v4l2_ext_controls ecs;
	struct v4l2_format f;
	struct { int fd; } ra;
	int c, r, vfd, mfd, accept = 0, bad = 0, want_sp, npic = 0;
	size_t len, pos = 0;
	uint8_t *data;
	unsigned s, sl = 0, nc = 0;
	FILE *fp;

	while ((c = getopt(argc, argv, "ai:v:m:")) != -1) {
		switch (c) {
		case 'a': accept = 1; break;
		case 'i': what = optarg; break;
		case 'v': vpath = optarg; break;
		case 'm': mpath = optarg; break;
		default: return (2);
		}
	}
	if (argc - optind != 1) {
		fprintf(stderr, "usage: cedrus_h264_reject [-a] [-i what] [-v video] [-m media] "
		    "stream.264\n");
		return (2);
	}
	memset(&flat_sm, 16, sizeof(flat_sm));
	fp = fopen(argv[optind], "rb");
	if (fp == NULL)
		die(argv[optind]);
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc(len + 16);
	if (fread(data, 1, len, fp) != len)
		die("stream");
	fclose(fp);

	/* Without the engine's limits; for an SP stream go on to its first SP slice. */
	hc->strict = 0;
	want_sp = what == NULL && strstr(argv[optind], "sp") != NULL;
	want_sp |= what == NULL && strstr(argv[optind], "SP") != NULL;
	for (;;) {
		r = h264_next_picture(hc, data, len, &pos, pic, (uint64_t)(npic + 1) * 1000);
		if (r != 1) {
			fprintf(stderr, "picture %d: parse failed (%d)\n", npic, r);
			return (2);
		}
		npic++;
		if (!want_sp)
			break;
		for (s = 0; s < pic->nslices; s++)
			if (pic->slice[s].sp.slice_type == V4L2_H264_SLICE_TYPE_SP)
				break;
		if (s < pic->nslices) {
			sl = s;
			break;
		}
	}
	if (what != NULL)
		inject(pic, what);
	printf("%s%s%s: picture %d slice %u: chroma_format_idc %u bit_depth %u/%u "
	    "separate_plane %d slice_groups %u slice_type %u colour_plane %u, %ux%u MBs%s\n",
	    argv[optind], what ? " +" : "", what ? what : "", npic - 1, sl,
	    pic->sps.chroma_format_idc, pic->sps.bit_depth_luma_minus8 + 8,
	    pic->sps.bit_depth_chroma_minus8 + 8,
	    (pic->sps.flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE) != 0,
	    pic->pps.num_slice_groups_minus1 + 1, pic->slice[sl].sp.slice_type,
	    pic->slice[sl].sp.colour_plane_id, pic->sps.pic_width_in_mbs_minus1 + 1,
	    pic->sps.pic_height_in_map_units_minus1 + 1,
	    (pic->sps.flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ? "" : " (map units)");

	vfd = open(vpath, O_RDWR);
	mfd = open(mpath, O_RDWR);
	if (vfd < 0 || mfd < 0)
		die("open");
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	f.fmt.pix.width = (pic->sps.pic_width_in_mbs_minus1 + 1) * 16;
	f.fmt.pix.height = (pic->sps.pic_height_in_map_units_minus1 + 1) * 16 *
	    ((pic->sps.flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ? 1 : 2);
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	if (ioctl(vfd, VIDIOC_S_FMT, &f) != 0)
		die("S_FMT out");

	/* The current SPS before anything is tried. */
	memset(&one, 0, sizeof(one));
	memset(&ecs, 0, sizeof(ecs));
	one.id = V4L2_CID_STATELESS_H264_SPS; one.ptr = &before; one.size = sizeof(before);
	ecs.which = V4L2_CTRL_WHICH_CUR_VAL; ecs.count = 1; ecs.controls = &one;
	if (ioctl(vfd, VIDIOC_G_EXT_CTRLS, &ecs) != 0)
		die("G_EXT_CTRLS sps");

	memset(ec, 0, sizeof(ec));
	ec[nc].id = V4L2_CID_STATELESS_H264_SPS; ec[nc].ptr = &pic->sps; ec[nc].size = sizeof(pic->sps); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_PPS; ec[nc].ptr = &pic->pps; ec[nc].size = sizeof(pic->pps); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_SCALING_MATRIX;
	ec[nc].ptr = pic->has_sm ? (void *)&pic->sm : (void *)&flat_sm; ec[nc].size = sizeof(flat_sm); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS;
	ec[nc].ptr = pic->slice[sl].has_pw ? (void *)&pic->slice[sl].pw : (void *)&no_pw;
	ec[nc].size = sizeof(no_pw); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_SLICE_PARAMS;
	ec[nc].ptr = &pic->slice[sl].sp; ec[nc].size = sizeof(pic->slice[sl].sp); nc++;
	ec[nc].id = V4L2_CID_STATELESS_H264_DECODE_PARAMS;
	ec[nc].ptr = &pic->dp; ec[nc].size = sizeof(pic->dp); nc++;

	memset(&ecs, 0, sizeof(ecs));
	ecs.which = V4L2_CTRL_WHICH_CUR_VAL; ecs.count = nc; ecs.controls = ec;
	bad += call(vfd, VIDIOC_TRY_EXT_CTRLS, &ecs, "TRY_EXT_CTRLS", accept);
	if (!accept) {
		/* On the current values: nothing may change. */
		bad += call(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS current", accept);
		memset(&ecs, 0, sizeof(ecs));
		one.ptr = &after;
		ecs.which = V4L2_CTRL_WHICH_CUR_VAL; ecs.count = 1; ecs.controls = &one;
		if (ioctl(vfd, VIDIOC_G_EXT_CTRLS, &ecs) != 0)
			die("G_EXT_CTRLS sps");
		r = memcmp(&before, &after, sizeof(before)) == 0;
		printf("  %-22s %s\n", "current SPS", r ? "unchanged" : "CHANGED");
		bad += !r;
	}
	memset(&ra, 0, sizeof(ra));
	if (ioctl(mfd, MEDIA_IOC_REQUEST_ALLOC, &ra) != 0)
		die("REQUEST_ALLOC");
	memset(&ecs, 0, sizeof(ecs));
	ecs.which = V4L2_CTRL_WHICH_REQUEST_VAL; ecs.request_fd = ra.fd;
	ecs.count = nc; ecs.controls = ec;
	bad += call(vfd, VIDIOC_S_EXT_CTRLS, &ecs, "S_EXT_CTRLS request", accept);
	close(ra.fd);
	printf("%s\n", bad == 0 ? (accept ? "ACCEPTED as expected" : "REJECTED as expected") :
	    "UNEXPECTED");
	return (bad != 0);
}
