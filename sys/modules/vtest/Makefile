# SPDX-License-Identifier: BSD-2-Clause
#
# vtest: a virtual capture device for exercising the media core. Build like
# sys/modules/media.

.PATH:	${.CURDIR}/../../dev/media

KMOD=	vtest
SRCS=	vtest.c
SRCS+=	vdev_if.h vbq_if.h
SRCS+=	device_if.h bus_if.h

CFLAGS+=	-I${.CURDIR}/../..
CFLAGS+=	-I${.CURDIR}/../../dev/media/shim
CFLAGS+=	-I${.CURDIR}/../../dev/media/uapi

.include <bsd.kmod.mk>
