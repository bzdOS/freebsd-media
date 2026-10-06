# SPDX-License-Identifier: BSD-2-Clause
#
# Allwinner A64 video engine (Cedrus). Build: tools/xbuild.sh cedrus
.PATH:	${.CURDIR}/../../dev/cedrus ${.CURDIR}/../../dev/media

KMOD=	cedrus
SRCS=	cedrus.c cedrus_dec.c cedrus_h264.c
SRCS+=	vdev_if.h vbq_if.h vm2m_if.h
SRCS+=	device_if.h bus_if.h ofw_bus_if.h opt_platform.h

CFLAGS+=	-I${.CURDIR}/../..
CFLAGS+=	-I${.CURDIR}/../../dev/media/shim
CFLAGS+=	-I${.CURDIR}/../../dev/media/uapi

.include <bsd.kmod.mk>
