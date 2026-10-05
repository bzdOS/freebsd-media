#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Build the media modules, push them to the guest's tmpfs (md5 verified: a
# module copied after a crash has been seen with the right size and bad
# content), unload what is loaded and load the new ones.
#
#   tools/guest-reload.sh            build, push, reload media + vtest
#   GUEST=root@host KEY=~/.ssh/k tools/guest-reload.sh
#
# Needs the guest up. Nothing is written to the guest's disks.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
GUEST=${GUEST:-root@192.0.2.82}
KEY=${KEY:-/root/.ssh/chimp_ed25519}
OBJ=${OBJ:-/opt/bzdos/fbsd-obj/media}
ssh_="ssh -i $KEY -o BatchMode=yes -o ConnectTimeout=8 $GUEST"

for m in media vtest; do "$here/xbuild.sh" $m >/dev/null || { echo "build of $m failed"; exit 1; }; done
scp -q -i "$KEY" "$OBJ/media/media.ko" "$OBJ/vtest/vtest.ko" "$GUEST:/tmp/"
for m in media vtest; do
	a=$(md5sum "$OBJ/$m/$m.ko" | cut -d' ' -f1)
	b=$($ssh_ "md5 -q /tmp/$m.ko" < /dev/null)
	[ "$a" = "$b" ] || { echo "md5 mismatch for $m.ko: $a != $b"; exit 1; }
done
$ssh_ 'kldstat -qn vtest && kldunload vtest; kldstat -qn media && kldunload media; kldload /tmp/media.ko && kldload /tmp/vtest.ko && dmesg | grep -E "^(vtest|vconv)0: \/dev" | tail -3; ls -l /dev/video*' < /dev/null
