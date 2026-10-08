#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# ffmpeg-test.sh [stream ...] — decode streams with FFmpeg's V4L2 Request hwaccel through our driver on the board
# and compare the raw output with the reference yuv (the software decode, as pushed by board-bootstrap.sh).
# Needs: the modules loaded (board-bootstrap.sh), FFmpeg built by build-ffmpeg.sh, streams in /tmp/ve.
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
STREAMS=${*:-"f3 smb wb t640"}
for s in $STREAMS; do
	$S "export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib; cd /tmp/ve;
	    rm -f $s.hw.yuv; /var/tmp/ffv4l2/bin/ffmpeg -v warning -hwaccel v4l2request -i $s.264 -an \
	      -pix_fmt yuv420p -f rawvideo -y $s.hw.yuv 2>&1 | tail -3;
	    if cmp -s $s.hw.yuv $s.yuv; then echo \"$s: EXACT\"; else echo \"$s: DIFFERENT (\$(cmp $s.hw.yuv $s.yuv 2>&1 | head -1))\"; fi" < /dev/null
done
