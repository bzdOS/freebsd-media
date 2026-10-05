#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Run v4l2-compliance on the guest against /dev/video0 (or $DEV). The tool is
# not installed in the guest's read-only root: the v4l-utils package and its
# dependencies are unpacked into /tmp/v4l (tmpfs) on first use.
#
#   tools/compliance.sh [v4l2-compliance args]       e.g. -s   (streaming)
set -eu
GUEST=${GUEST:-root@192.0.2.82}
KEY=${KEY:-/root/.ssh/chimp_ed25519}
DEV=${DEV:-/dev/video0}
ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=8 "$GUEST" "
	if [ ! -x /tmp/v4l/root/usr/local/bin/v4l2-compliance ]; then
		mkdir -p /tmp/v4l/pkgs /tmp/v4l/root &&
		pkg fetch -y -d -o /tmp/v4l/pkgs v4l-utils >/dev/null &&
		for f in \$(find /tmp/v4l/pkgs -name '*.pkg'); do tar -C /tmp/v4l/root -xf \$f 2>/dev/null; done
	fi
	cd /tmp/v4l/root/usr/local && LD_LIBRARY_PATH=lib timeout \${TMO:-300} bin/v4l2-compliance -d $DEV $* 2>&1
" < /dev/null
