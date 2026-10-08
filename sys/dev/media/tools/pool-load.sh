#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# pool-load.sh — build media+cedrus, push them (md5-verified) and reload only those two on the guest
# (the GPU stack stays loaded). OBJ=<objdir> as for board-bootstrap.sh. Run under flock.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OBJ=${OBJ:-/opt/bzdos/fbsd-obj/media}
K=/root/.ssh/chimp_ed25519; G=root@192.0.2.82
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
for m in media cedrus; do LC_ALL=C OBJ=$OBJ/$m $HERE/xbuild.sh $m >/dev/null; done
scp -q -i $K $OBJ/media/media.ko $OBJ/cedrus/cedrus.ko $G:/tmp/
for n in media cedrus; do
	a=$(md5sum $OBJ/$n/$n.ko | cut -d' ' -f1); b=$($S "md5 -q /tmp/$n.ko" </dev/null)
	[ "$a" = "$b" ] || { echo "MD5 MISMATCH $n"; exit 1; }; echo "$n.ko $a ok"
done
$S 'kldstat -qn cedrus && kldunload cedrus; kldstat -qn media && kldunload media;
    kldload /tmp/media.ko && kldload /tmp/cedrus.ko; kldstat | grep -E "media|cedrus"; dmesg | tail -4;
    sysctl dev.cedrus.0 | grep pool' </dev/null
