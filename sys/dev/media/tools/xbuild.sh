#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Cross-build a media kernel module for the arm64 guest on the Linux host.
#
#   tools/xbuild.sh media|vtest [make args...]      e.g. clean
#
# The kernel headers must be the ones the guest kernel was built from, or the
# module loads and then reads the wrong struct offsets (see the struct device
# ABI note in the bsdOS docs). Override with FBSD_SRC=<tree>. Objects go to
# $OBJ (default /opt/bzdos/fbsd-obj/media/<module>), the .ko is printed.
set -eu
mod=${1:?usage: xbuild.sh media|vtest [make args]}; shift
here=$(cd "$(dirname "$0")" && pwd)
FBSD_SRC=${FBSD_SRC:-/opt/bzdos/freebsd-src-earlyboot-wt}
OBJ=${OBJ:-/opt/bzdos/fbsd-obj/media/$mod}
mkdir -p "$OBJ"
cd "$here/../../../modules/$mod"
LC_ALL=C MAKEOBJDIR=$OBJ bmake \
    -m "$FBSD_SRC/share/mk" \
    MACHINE=arm64 MACHINE_ARCH=aarch64 SYSDIR="$FBSD_SRC/sys" \
    CC="clang --target=aarch64-unknown-freebsd15.1" LD=ld.lld \
    COMPILER_TYPE=clang COMPILER_VERSION=210100 \
    XARGS_J=-I OBJCOPY=llvm-objcopy NM=llvm-nm "$@"
[ "${1:-}" = clean ] || echo "built: $OBJ/$mod.ko"
