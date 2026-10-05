#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Run v4l2-compliance on the guest against /dev/video0 (or $DEV). The tools are
# not installed in the guest's read-only root: the v4l-utils package and its
# dependencies are unpacked into /tmp/v4l (tmpfs, gone at every reboot) on first
# use, and v4l2-compliance/v4l2-ctl are patched there to recognise mem2mem
# devices (tools/v4l-m2m-patch.py; the port's detection only knows capture).
#
#   tools/compliance.sh [v4l2-compliance args]       e.g. -s   (streaming)
#   DEV=/dev/video1 tools/compliance.sh -s           the mem2mem converter
#   tools/compliance.sh --ctl [v4l2-ctl args]        v4l2-ctl instead
set -eu
here=$(cd "$(dirname "$0")" && pwd)
GUEST=${GUEST:-root@192.0.2.82}
KEY=${KEY:-/root/.ssh/chimp_ed25519}
DEV=${DEV:-/dev/video0}
ssh_="ssh -i $KEY -o BatchMode=yes -o ConnectTimeout=8 $GUEST"
tool=v4l2-compliance
if [ "${1:-}" = --ctl ]; then tool=v4l2-ctl; shift; fi
L=/tmp/v4l/root/usr/local

if ! $ssh_ "test -x $L/bin/$tool.m2m" < /dev/null; then
	$ssh_ "
		if [ ! -x $L/bin/v4l2-compliance ]; then
			mkdir -p /tmp/v4l/pkgs /tmp/v4l/root &&
			pkg fetch -y -d -o /tmp/v4l/pkgs v4l-utils >/dev/null &&
			for f in \$(find /tmp/v4l/pkgs -name '*.pkg'); do tar -C /tmp/v4l/root -xf \$f 2>/dev/null; done
		fi" < /dev/null
	tmp=$(mktemp -d)
	scp -q -i "$KEY" "$GUEST:$L/bin/$tool" "$tmp/$tool"
	"$here/v4l-m2m-patch.py" "$tmp/$tool" "$tmp/$tool.m2m" >/dev/null
	chmod +x "$tmp/$tool.m2m"
	scp -q -i "$KEY" "$tmp/$tool.m2m" "$GUEST:$L/bin/$tool.m2m"
	rm -rf "$tmp"
fi
# The tools find a video node's media device through Linux sysfs; this answers
# that listing from the media devices' topology (tools/mi-sysfs-shim.c).
if ! $ssh_ "test -f /tmp/v4l/mi-sysfs-shim.so" < /dev/null; then
	tmp=$(mktemp -d)
	clang --target=aarch64-unknown-freebsd15.1 \
	    --sysroot="${SYSROOT:-/opt/bzdos/sysroots/aarch64-15.1}" -fuse-ld=lld \
	    -shared -fPIC -O2 -Wall -I"$here/../shim" -I"$here/../uapi" \
	    -o "$tmp/mi-sysfs-shim.so" "$here/mi-sysfs-shim.c"
	scp -q -i "$KEY" "$tmp/mi-sysfs-shim.so" "$GUEST:/tmp/v4l/mi-sysfs-shim.so"
	rm -rf "$tmp"
fi
case $tool in
v4l2-compliance) args="-d $DEV $*" ;;
*) args="-d $DEV $*" ;;
esac
$ssh_ "cd $L && LD_PRELOAD=/tmp/v4l/mi-sysfs-shim.so LD_LIBRARY_PATH=lib timeout \${TMO:-300} bin/$tool.m2m $args 2>&1" < /dev/null
