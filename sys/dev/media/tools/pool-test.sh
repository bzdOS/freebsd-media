#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# pool-test.sh frag|cycles [MB|N] — F:M041 proofs on the board (run under flock).
#   frag [MB]   fragment physical memory with tests/frag.c (default 500 MB), then decode 1080p
#               three times through FFmpeg; print pool counters and the verdict.
#   cycles [N]  N (20) times: ffmpeg opens the device, decodes 10 frames of 1080p and closes it;
#               print dev.cedrus.0.pool_used_kb before, after and the live/fail counters.
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
HERE=$(cd "$(dirname "$0")" && pwd)
FF='export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib; /var/tmp/ffv4l2/bin/ffmpeg -v warning -hwaccel v4l2request -hwaccel_output_format drm_prime'
POOL='sysctl -n dev.cedrus.0.pool_kb dev.cedrus.0.pool_used_kb dev.cedrus.0.pool_largest_kb dev.cedrus.0.pool_live dev.cedrus.0.pool_fails dev.cedrus.0.pool_allocs 2>&1 | tr "\n" " "'
case ${1:-} in
frag)
	scp -q -i $K $HERE/../tests/frag.c $G:/tmp/ve/ && $S 'cc -o /tmp/ve/frag /tmp/ve/frag.c' </dev/null || exit 1
	$S "sysctl vm.stats.vm.v_free_count | tr '\n' ' '; (/tmp/ve/frag ${2:-500} >/tmp/ve/frag.out 2>&1 </dev/null &) ; sleep 8; cat /tmp/ve/frag.out; sysctl vm.stats.vm.v_free_count
	  echo pool before: \$($POOL)
	  cd /var/tmp; for i in 1 2 3; do $FF -i p1080.264 -f null - 2>&1 | tail -3; echo run \$i done; done
	  echo pool after: \$($POOL)
	  pkill -f 've/frag'; sleep 1; echo frag killed" </dev/null ;;
cycles)
	n=${2:-20}
	$S "echo before: \$($POOL); cd /var/tmp; ok=0; bad=0
	  for i in \$(seq $n); do
	    if $FF -i p1080.264 -frames:v 10 -f null - >/tmp/ve/cyc.out 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); cat /tmp/ve/cyc.out | tail -2; fi
	    done; echo cycles ok=\$ok bad=\$bad; echo after: \$($POOL)" </dev/null ;;
*) echo "usage: pool-test.sh frag|cycles [n]"; exit 2 ;;
esac
