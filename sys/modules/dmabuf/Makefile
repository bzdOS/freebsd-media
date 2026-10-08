# SPDX-License-Identifier: BSD-2-Clause
#
# The dmabuf core (sys/dev/dmabuf). Named dmabuf_base while drm-kmod still
# ships a module called dmabuf with its own, different ABI; the two must not be
# confused by a MODULE_DEPEND. Build like sys/modules/media.

.PATH:	${.CURDIR}/../../dev/dmabuf

KMOD=	dmabuf_base
SRCS=	dmabuf.c
SRCS+=	device_if.h bus_if.h

CFLAGS+=	-I${.CURDIR}/../..

# Out-of-tree modules install under /boot/modules (stock modules: /boot/kernel).
KMODDIR?=	/boot/modules

.include <bsd.kmod.mk>
