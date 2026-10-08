# SPDX-License-Identifier: BSD-2-Clause
#
# vtest: virtual devices for exercising the media core: a camera (vtest) and a
# mem2mem converter (vconv). Build like sys/modules/media.

.PATH:	${.CURDIR}/../../dev/media

KMOD=	vtest
SRCS=	vtest.c vconv.c
SRCS+=	vdev_if.h vbq_if.h vm2m_if.h
SRCS+=	device_if.h bus_if.h

CFLAGS+=	-I${.CURDIR}/../..
CFLAGS+=	-I${.CURDIR}/../../dev/media/shim
CFLAGS+=	-I${.CURDIR}/../../dev/media/uapi

# Out-of-tree modules install under /boot/modules (stock modules: /boot/kernel).
KMODDIR?=	/boot/modules

.include <bsd.kmod.mk>
