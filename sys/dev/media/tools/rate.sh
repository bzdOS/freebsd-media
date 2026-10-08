# SPDX-License-Identifier: BSD-2-Clause
# usage: sh rate.sh runs stream   -> per-slice error rate from the driver's counters
cd /tmp/ve
sl() { sysctl -n dev.cedrus.0.h264_last | sed 's/.*slices \([0-9]*\).*/\1/'; }
e0=$(sysctl -n dev.cedrus.0.errors); s0=$(sl)
i=0; while [ $i -lt $1 ]; do ./cedrus_h264 -n 3 $2.264 $2.yuv >/dev/null 2>&1; i=$((i+1)); done
e1=$(sysctl -n dev.cedrus.0.errors); s1=$(sl)
echo "$2 runs=$1 slices=$((s1-s0)) errors=$((e1-e0))"
