#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Cross-build a media kernel module for the arm64 guest on the Linux host.
#
#   tools/xbuild.sh dmabuf|media|vtest [make args...]      e.g. clean
#
# The kernel headers must be the ones the guest kernel was built from, or the
# module loads and then reads the wrong struct offsets (see the struct device
# ABI note in the bsdOS docs). Override with FBSD_SRC=<tree>. Objects go to
# $OBJ (default /opt/bzdos/fbsd-obj/media/<module>), the .ko is printed.
set -eu
mod=${1:?usage: xbuild.sh dmabuf|media|vtest [make args]}; shift
here=$(cd "$(dirname "$0")" && pwd)
FBSD_SRC=${FBSD_SRC:-/opt/bzdos/freebsd-src-earlyboot-wt}
OBJ=${OBJ:-/opt/bzdos/fbsd-obj/media/$mod}
mkdir -p "$OBJ"
# "xbuild.sh <module> install DESTDIR=<staging root>": install with GNU install through a wrapper and
# without kldxref (it is a FreeBSD binary; run it on the target, see docs/INSTALL.md).
extra=
for a in "$@"; do
	[ "$a" = install ] && extra="INSTALL=$here/gnu-install-wrap.sh NO_XREF=yes"
done
cd "$here/../../../modules/$mod"
LC_ALL=C MAKEOBJDIR=$OBJ bmake \
    -m "$FBSD_SRC/share/mk" \
    MACHINE=arm64 MACHINE_ARCH=aarch64 SYSDIR="$FBSD_SRC/sys" \
    CC="clang --target=aarch64-unknown-freebsd15.1" LD=ld.lld \
    COMPILER_TYPE=clang COMPILER_VERSION=210100 \
    XARGS_J=-I OBJCOPY=llvm-objcopy NM=llvm-nm $extra "$@"
ko=$mod; [ "$mod" = dmabuf ] && ko=dmabuf_base
[ "${1:-}" = clean ] || [ -n "$extra" ] || echo "built: $OBJ/$ko.ko"
