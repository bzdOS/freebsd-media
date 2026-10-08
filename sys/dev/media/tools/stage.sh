#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Cross-build the media modules for aarch64 and install them, the man pages and the configuration
# samples into a staging root:
#
#   DESTDIR=/path/to/stage [FBSD_SRC=...] [OBJ=...] [PREFIX=/usr/local] [MODULES="dmabuf media cedrus"] \
#       sys/dev/media/tools/stage.sh
#
# FBSD_SRC must be the source tree the target kernel was built from (see docs/INSTALL.md, "ABI").
# OBJ is the object directory prefix (default /opt/bzdos/fbsd-obj/media-stage); each module gets a
# subdirectory. Result, relative to DESTDIR:
#   boot/modules/{dmabuf_base,media,cedrus}.ko
#   boot/loader.conf.d/v4l2-cedrus.conf        (inactive until the owner copies the stage to the target)
#   <PREFIX>/share/man/man4, man7              the man pages
#   <PREFIX>/share/examples/freebsd-media/     rc.conf.d/kld and devfs.rules samples
# kldxref is not run here (it is a FreeBSD tool): run "kldxref /boot/modules" on the target.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../../.." && pwd)
: "${DESTDIR:?set DESTDIR=<staging root>}"
PREFIX=${PREFIX:-/usr/local}
MODULES=${MODULES:-"dmabuf media cedrus"}
OBJBASE=${OBJ:-/opt/bzdos/fbsd-obj/media-stage}
mkdir -p "$DESTDIR"
for m in $MODULES; do
	OBJ=$OBJBASE/$m LC_ALL=C "$here/xbuild.sh" "$m"
	OBJ=$OBJBASE/$m LC_ALL=C "$here/xbuild.sh" "$m" install DESTDIR="$DESTDIR" >/dev/null
done
ex=$DESTDIR$PREFIX/share/examples/freebsd-media
mkdir -p "$DESTDIR/boot/loader.conf.d" "$ex" "$DESTDIR$PREFIX/share/man/man4" "$DESTDIR$PREFIX/share/man/man7"
install -m 644 "$top/etc/loader.conf.d/v4l2-cedrus.conf" "$DESTDIR/boot/loader.conf.d/"
install -m 644 "$top/etc/rc.conf.d/kld" "$top/etc/devfs.rules" "$ex/"
install -m 444 "$top"/share/man/man4/*.4 "$DESTDIR$PREFIX/share/man/man4/"
install -m 444 "$top"/share/man/man7/*.7 "$DESTDIR$PREFIX/share/man/man7/"
echo "staged into $DESTDIR:"
(cd "$DESTDIR" && find . -type f | sort)
