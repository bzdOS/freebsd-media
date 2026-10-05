# SPDX-License-Identifier: BSD-2-Clause
#
# The media core: /dev/videoN nodes, the buffer queue and the controls
# framework (sys/dev/media). Drivers depend on this module.
#
# Cross-build on the Linux host, same recipe as hal/lima (the headers must be
# the ones the guest kernel was built from):
#
#   MAKEOBJDIRPREFIX=/opt/bzdos/fbsd-obj bmake \
#       -m /opt/bzdos/freebsd-src-earlyboot-wt/share/mk \
#       MACHINE=arm64 MACHINE_ARCH=aarch64 \
#       SYSDIR=/opt/bzdos/freebsd-src-earlyboot-wt/sys \
#       CC="clang --target=aarch64-unknown-freebsd15.1" LD=ld.lld \
#       COMPILER_TYPE=clang COMPILER_VERSION=210100 \
#       XARGS_J=-I OBJCOPY=llvm-objcopy NM=llvm-nm

.PATH:	${.CURDIR}/../../dev/media

KMOD=	media
SRCS=	vdev.c vbq.c vctrl.c vevent.c vm2m.c vmedia.c vreq.c
SRCS+=	vdev_if.c vdev_if.h vbq_if.c vbq_if.h vm2m_if.c vm2m_if.h
SRCS+=	device_if.h bus_if.h

# <dev/media/...> lives next to this module, not in ${SYSDIR}.
CFLAGS+=	-I${.CURDIR}/../..
CFLAGS+=	-I${.CURDIR}/../../dev/media/shim
CFLAGS+=	-I${.CURDIR}/../../dev/media/uapi

.include <bsd.kmod.mk>
