#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# ffmpeg-test.sh [stream ...] — decode streams with FFmpeg's V4L2 Request hwaccel through our driver on the board
# and compare the raw output with the reference yuv (the software decode, as pushed by board-bootstrap.sh).
# Needs: the modules loaded (board-bootstrap.sh), FFmpeg built by build-ffmpeg.sh, streams in /tmp/ve.
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
TOL=${TOL:-0}	# MPEG-2's IDCT is not bit-exact (M038): TOL=2 and EXT=m2v for MPEG-2 streams
EXT=${EXT:-264}
STREAMS=${*:-"f3 smb wb t640"}
for s in $STREAMS; do
	$S "export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib; cd /tmp/ve;
	    rm -f $s.hw.yuv; /var/tmp/ffv4l2/bin/ffmpeg -v warning -hwaccel v4l2request -i $s.$EXT -an \
	      -pix_fmt yuv420p -f rawvideo -y $s.hw.yuv 2>&1 | tail -3;
	    [ -x /tmp/ve/cmpyuv ] || cc -O2 -o /tmp/ve/cmpyuv /tmp/ve/cmpyuv.c; if /tmp/ve/cmpyuv $s.hw.yuv $s.yuv $TOL > /tmp/ve/cmp.out; then echo \"$s: OK (tolerance $TOL): \$(cat /tmp/ve/cmp.out)\"; else echo \"$s: DIFFERENT: \$(cat /tmp/ve/cmp.out)\"; fi" < /dev/null
done
