#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# FreeBSD's bsd.kmod.mk calls "install -T release -o root -g wheel ...". GNU install on a Linux
# cross host knows neither -T nor the group "wheel". This wrapper drops those options (a staging
# root is owned by whoever runs the build; ownership is restored when the tree is unpacked on the
# target as root, see docs/INSTALL.md) and passes the rest to GNU install.
args=
while [ $# -gt 0 ]; do
	case "$1" in
	-T|-o|-g) shift 2 ;;
	*) args="$args '$1'"; shift ;;
	esac
done
# the destination directory of a staging root may not exist yet
eval "set -- $args"
for last; do :; done
case "$last" in */) mkdir -p "$last" ;; esac
eval exec install $args
