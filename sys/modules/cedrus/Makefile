# SPDX-License-Identifier: BSD-2-Clause
#
# Allwinner A64 video engine (Cedrus). Build: tools/xbuild.sh cedrus
.PATH:	${.CURDIR}/../../dev/cedrus

KMOD=	cedrus
SRCS=	cedrus.c
SRCS+=	device_if.h bus_if.h ofw_bus_if.h opt_platform.h 

CFLAGS+=	-I${.CURDIR}/../..
CFLAGS+=	-I${.CURDIR}/../../dev/media/shim
CFLAGS+=	-I${.CURDIR}/../../dev/media/uapi

.include <bsd.kmod.mk>
