#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# speed-test.sh [stream.264 ...] — how fast does the board decode? For each stream (in /var/tmp on the guest):
#   hw-nocopy   FFmpeg + our driver, frames stay in DRM_PRIME dma-bufs (-hwaccel_output_format drm_prime), null sink
#   hw-readback FFmpeg + our driver, frames copied out to system memory (the default of -hwaccel)
#   sw          libavcodec on the CPU (the guest's pkg ffmpeg, FFMPEG_SW=/var/tmp/ff/root/usr/local)
# Prints wall time, user and system CPU time and the frame rate (ffmpeg -benchmark).
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
STREAMS=${*:-"p480 p720 p1080"}
SW=${FFMPEG_SW:-/var/tmp/ff/root/usr/local}
for s in $STREAMS; do
	n=$($S "/var/tmp/ffv4l2/bin/ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 /var/tmp/$s.264 2>/dev/null" </dev/null | head -1)
	echo "== $s ($n frames)"
	for mode in hw-nocopy hw-readback sw; do
		case $mode in
		hw-nocopy)   cmd="LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib /var/tmp/ffv4l2/bin/ffmpeg -v info -benchmark -hwaccel v4l2request -hwaccel_output_format drm_prime -i /var/tmp/$s.264 -f null -" ;;
		hw-readback) cmd="LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib /var/tmp/ffv4l2/bin/ffmpeg -v info -benchmark -hwaccel v4l2request -i /var/tmp/$s.264 -pix_fmt nv12 -f null -" ;;
		sw)          cmd="LD_LIBRARY_PATH=$SW/lib $SW/bin/ffmpeg -v info -benchmark -threads 4 -i /var/tmp/$s.264 -pix_fmt nv12 -f null -" ;;
		esac
		out=$($S "$cmd 2>&1 | grep -E 'bench|frame=' | tr '\\r\\n' '  '" </dev/null)
		echo "  $mode: $(echo "$out" | sed -e "s/bench: //g" -e "s/ \\+/ /g" | grep -oE "frame= *[0-9]+ .*elapsed=[0-9:.]+|utime=[^ ]+ stime=[^ ]+ rtime=[^ ]+" | tr "\n" " ")"
	done
done
