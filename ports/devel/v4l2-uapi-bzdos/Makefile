# UNTESTED: written on a Linux host without a ports tree or poudriere. See the notes of
# devel/libudev-bzdos about the distfile (private repository, placeholder distinfo).
#
# The Linux V4L2 / media / dma-buf user interface headers FFmpeg's V4L2 request hwaccel includes
# (<linux/videodev2.h>, <linux/v4l2-controls.h>, <linux/media.h>, <linux/dma-buf.h>, ...), as
# sys/dev/media/uapi, and the three-line shims they need (<linux/types.h>, <linux/ioctl.h>,
# <linux/compiler.h>, <linux/const.h>) from sys/dev/media/shim. They are installed in a private
# directory, to be added with -I, so that they never shadow a <linux/...> directory of another port.

PORTNAME=	v4l2-uapi-bzdos
DISTVERSION=	0.1
CATEGORIES=	devel

MAINTAINER=	ap.bodrov@gmail.com
COMMENT=	Linux V4L2 and media user interface headers for the freebsd-media stack
WWW=		https://github.com/bzdOS/freebsd-media

LICENSE=	BSD2CLAUSE BSD3CLAUSE
LICENSE_COMB=	multi

USE_GITHUB=	yes
GH_ACCOUNT=	bzdOS
GH_PROJECT=	freebsd-media
GH_TAGNAME=	v${DISTVERSION}

NO_ARCH=	yes
NO_BUILD=	yes
INCDIR=		${PREFIX}/include/v4l2-uapi-bzdos
PLIST_SUB=	INCDIR=include/v4l2-uapi-bzdos

do-install:
	${MKDIR} ${STAGEDIR}${INCDIR}/linux
	${INSTALL_DATA} ${WRKSRC}/sys/dev/media/uapi/linux/*.h ${STAGEDIR}${INCDIR}/linux/
	${INSTALL_DATA} ${WRKSRC}/sys/dev/media/shim/linux/*.h ${STAGEDIR}${INCDIR}/linux/

.include <bsd.port.mk>
