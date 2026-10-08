#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# board-bootstrap.sh [stream ...] — put the whole H.264 test rig back on the board.
#
# The guest's /tmp is a tmpfs, so every reboot (a crash, the watchdog, an engine reset
# that hangs the bus) takes the modules, the test binary and the streams with it.  This
# script rebuilds that state from the host in one go: the three modules (md5 verified),
# the headers the test needs, the test and the lab interpreter compiled on the guest,
# and the streams with their ffmpeg references.
#
#   board-bootstrap.sh                     # the usual set
#   board-bootstrap.sh wb smb hd720        # a smaller one
set -eu
R=$(cd "$(dirname "$0")/../../../.." && pwd)
M=$R/sys/dev/media
C=$R/sys/dev/cedrus
OBJ=${OBJ:-/opt/bzdos/fbsd-obj/media}	# a worktree build keeps its own objects: OBJ=<dir>
ST=/opt/bzdos/linux-work/initramfs-h264/streams
K=/root/.ssh/chimp_ed25519
G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 $G"
STREAMS=${*:-"f1 f2 f3 wb smb hd640 hd720 t640"}

echo "== host: build what is stale"
for m in dmabuf media cedrus; do LC_ALL=C OBJ=$OBJ/$m $M/tools/xbuild.sh $m >/dev/null; done

echo "== push modules, sources, headers, streams"
$S 'mkdir -p /tmp/ve /tmp/shim /tmp/uapi'
scp -q -i $K $OBJ/dmabuf/dmabuf_base.ko $OBJ/media/media.ko $OBJ/cedrus/cedrus.ko $G:/tmp/
scp -q -r -i $K $M/shim/linux $G:/tmp/shim/
scp -q -r -i $K $M/uapi/linux $G:/tmp/uapi/
scp -q -i $K $M/tests/cedrus_h264.c $M/tests/h264ctl.h $G:/tmp/ve/
scp -q -i $K $C/lab/ve_lab.c $C/cedrus_lab.h $G:/tmp/ve/
scp -q -i $K $C/cedrus_lab.h $G:/tmp/
for s in $STREAMS; do
	scp -q -i $K $ST/$s.264 $G:/tmp/ve/ 2>/dev/null || echo "  (нет $s.264)"
	scp -q -i $K $ST/$s.yuv $G:/tmp/ve/ 2>/dev/null || echo "  (нет $s.yuv)"
done

echo "== verify the modules and build on the guest"
for m in "dmabuf dmabuf_base" "media media" "cedrus cedrus"; do
	set -- $m; d=$1; n=$2
	a=$(md5sum $OBJ/$d/$n.ko | cut -d' ' -f1)
	b=$($S "md5 -q /tmp/$n.ko" < /dev/null)
	[ "$a" = "$b" ] || { echo "MD5 MISMATCH $n.ko: $a != $b"; exit 1; }
	echo "  $n.ko $a ok"
done
$S 'cd /tmp/ve && cc -I/tmp/shim -I/tmp/uapi -o cedrus_h264 cedrus_h264.c &&
    cc -o ve_lab ve_lab.c && echo "  cedrus_h264 and ve_lab built"' < /dev/null

echo "== load (dmabuf_base -> media -> cedrus)"
$S 'kldstat -qn cedrus && kldunload cedrus; kldstat -qn media && kldunload media;
    kldstat -qn dmabuf_base && kldunload dmabuf_base;
    kldload /tmp/dmabuf_base.ko && kldload /tmp/media.ko && kldload /tmp/cedrus.ko &&
    echo "  loaded"; ls /dev/video0 /dev/media0 2>/dev/null; sysctl -n dev.cedrus.0.pictures 2>/dev/null' < /dev/null

echo "== ready: /tmp/ve/cedrus_h264, /tmp/ve/ve_lab, streams: $STREAMS"
