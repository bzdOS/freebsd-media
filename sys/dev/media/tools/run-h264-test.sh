#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# run-h264-test.sh [stream ...] — push the media + cedrus modules and the H.264
# test to the guest, load them and compare decodes with the reference YUV.
#
#   run-h264-test.sh                 # smb (small), wb (M015 stream), f3
#   run-h264-test.sh wb
#
# The test binary is built on the guest (cc -I/tmp/shim -I/tmp/uapi), the recipe
# the test's own header states.  Nothing is written to the guest's disks: all
# paths are in /tmp.
set -eu
R=$(cd "$(dirname "$0")/../../../.." && pwd)
M=$R/sys/dev/media
ST=/opt/bzdos/linux-work/initramfs-h264/streams
OBJ=/opt/bzdos/fbsd-obj/media
K=/root/.ssh/chimp_ed25519
# Guest address: $BZDK_GUEST_IP wins, else ask the HV via bzd_board (DHCP on
# 198.51.100.0/24 since the 2026-10-12 cutover), else the pre-cutover static address.
G=root@${BZDK_GUEST_IP:-$(python3 -c 'import sys; sys.path.insert(0,"/opt/bzdos/microkernel"); import bzd_board; print(bzd_board.guest_ip())' 2>/dev/null || echo 192.0.2.82)}
S="ssh -i $K -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 $G"
STREAMS=${*:-"smb wb f3"}

echo "== push sources, headers, modules"
$S 'mkdir -p /tmp/ve /tmp/shim /tmp/uapi'
scp -q -r -i $K $M/shim/linux $G:/tmp/shim/
scp -q -r -i $K $M/uapi/linux $G:/tmp/uapi/
scp -q -i $K $M/tests/cedrus_h264.c $M/tests/h264ctl.h $G:/tmp/ve/
scp -q -i $K $OBJ/media/media.ko $OBJ/cedrus/cedrus.ko $OBJ/dmabuf/dmabuf_base.ko $G:/tmp/
for s in $STREAMS; do scp -q -i $K $ST/$s.264 $ST/$s.yuv $G:/tmp/ve/; done

echo "== md5 of the pushed modules"
for m in "media media" "cedrus cedrus" "dmabuf dmabuf_base"; do
	set -- $m; d=$1; n=$2
	a=$(md5sum $OBJ/$d/$n.ko | cut -d' ' -f1)
	b=$($S "md5 -q /tmp/$n.ko" < /dev/null)
	[ "$a" = "$b" ] || { echo "MD5 MISMATCH $n.ko: $a != $b"; exit 1; }
	echo "  $n.ko $a ok"
done

echo "== build the test on the guest"
$S 'cd /tmp/ve && cc -I/tmp/shim -I/tmp/uapi -o cedrus_h264 cedrus_h264.c' < /dev/null

echo "== load dmabuf_base + media + cedrus (media depends on dmabuf_base)"
$S 'kldstat -qn cedrus && kldunload cedrus; kldstat -qn media && kldunload media;
    kldstat -qn dmabuf_base && kldunload dmabuf_base;
    kldload /tmp/dmabuf_base.ko && kldload /tmp/media.ko && kldload /tmp/cedrus.ko && echo loaded;
    dmesg | grep -E "cedrus0|media0|dmabuf" | tail -6; ls /dev/video* /dev/media* 2>/dev/null' < /dev/null

echo "== decode and compare"
for s in $STREAMS; do
	echo "-- $s"
	$S "cd /tmp/ve && ./cedrus_h264 -n 3 $s.264 $s.yuv 2>&1 | tail -3" < /dev/null || true
done
