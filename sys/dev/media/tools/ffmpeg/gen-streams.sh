#!/bin/bash
# Generate the speed-test streams (M040) with x264 and push them to the guest's /var/tmp.
set -eu
D=${D:-/opt/bzdos/linux-work/ffmpeg/perf}; mkdir -p $D; cd $D
gen() { ffmpeg -y -v error -f lavfi -i "mandelbrot=size=$2:rate=30" -frames:v $3 -c:v libx264 -preset medium -crf 24 -profile:v $4 -bf 2 -refs 3 -g 60 -pix_fmt yuv420p -f h264 $1.264; }
gen p480 720x480 300 main; gen p720 1280x720 300 high; gen p1080 1920x1088 150 high
scp -q -i /root/.ssh/chimp_ed25519 p480.264 p720.264 p1080.264 root@192.0.2.82:/var/tmp/
