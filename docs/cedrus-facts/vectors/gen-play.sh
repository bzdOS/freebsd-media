#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# gen-play.sh [outdir] -- regenerate play.m2v (60 pictures, 352x288, I and P only) and its reference decode.
# Regenerated 2026-10-09: the original file's command was never recorded. The content is FFmpeg's synthetic
# testsrc2 pattern, so nothing third-party is involved.
set -eu
O=${1:-.}
ffmpeg -v error -y -f lavfi -i "testsrc2=size=352x288:rate=25" -frames:v 60 -c:v mpeg2video -q:v 5 -g 12 -bf 0 \
    -pix_fmt yuv420p -f mpeg2video "$O/play.m2v"
ffmpeg -v error -y -i "$O/play.m2v" -f rawvideo -pix_fmt yuv420p "$O/play.yuv"
ls -l "$O/play.m2v" "$O/play.yuv"; md5sum "$O/play.m2v" "$O/play.yuv"
