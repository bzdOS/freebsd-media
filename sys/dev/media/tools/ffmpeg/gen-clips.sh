#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# gen-clips.sh [outdir] — test material for fbplay, made on the host with ffmpeg/libx264: 60 s, 30 fps, a moving test
# pattern with the frame number and time burned in, and a 1 kHz beep of 100 ms at the start of every second (stereo,
# 48 kHz). c360/c720/c1080.mp4 are H.264 High with 2 B-frames and AAC; m360/m720/m1080.ts are MPEG-2 with MP2 in
# MPEG-TS; c360.mkv is H.264 + MP3; c360.264 and m360.m2v are raw elementary streams.
set -eu
O=${1:-/opt/bzdos/linux-work/ffmpeg/clips}
mkdir -p "$O"; cd "$O"
FONT=${FONT:-/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf}
beep='aevalsrc=0.5*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.1)|0.5*sin(2*PI*1000*t)*lt(mod(t\,1)\,0.1):s=48000:d=60'
vid() {		# w h
	echo "testsrc2=s=$1x$2:r=30:d=60,drawtext=fontfile='$FONT':text='%{frame_num} %{pts\:hms}':x=(w-tw)/2:y=h-th-8:fontsize=h/10:fontcolor=white:box=1:boxcolor=black@0.6,format=yuv420p"
}
for r in 360:640 720:1280 1080:1920; do
	h=${r%:*}; w=${r#*:}
	ffmpeg -v error -y -f lavfi -i "$(vid $w $h)" -f lavfi -i "$beep" -c:v libx264 -profile:v high -bf 2 -refs 3 -g 60 \
	    -preset medium -crf 23 -c:a aac -b:a 128k -movflags +faststart -t 60 c$h.mp4
	ffmpeg -v error -y -f lavfi -i "$(vid $w $h)" -f lavfi -i "$beep" -c:v mpeg2video -b:v 4M -bf 2 -g 15 -c:a mp2 \
	    -b:a 192k -f mpegts -t 60 m$h.ts
done
ffmpeg -v error -y -f lavfi -i "$(vid 640 360)" -f lavfi -i "$beep" -c:v libx264 -profile:v high -bf 2 -g 60 -crf 23 \
    -c:a libmp3lame -b:a 128k -t 60 c360.mkv
ffmpeg -v error -y -i c360.mp4 -an -c:v copy -bsf:v h264_mp4toannexb c360.264
ffmpeg -v error -y -i m360.ts -an -c:v copy m360.m2v
ls -l
