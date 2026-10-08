#!/bin/bash
# pool-old.sh — contrast run: 1080p through FFmpeg on whatever cedrus is loaded now
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
$S 'kldstat | grep -E "cedrus|media"; md5 -q /tmp/cedrus.ko; sysctl vm.stats.vm.v_free_count; export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib; cd /var/tmp; for i in 1 2 3; do /var/tmp/ffv4l2/bin/ffmpeg -v warning -hwaccel v4l2request -hwaccel_output_format drm_prime -i p1080.264 -f null - 2>&1 | tail -4; echo "run $i rc=$?"; done' </dev/null
