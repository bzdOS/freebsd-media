# UNTESTED: written on a Linux host without a ports tree or poudriere (the shim itself is built and
# tested by sys/dev/media/tools/ffmpeg/build-ffmpeg.sh, "stage"). Run portlint and a poudriere build.
#
# A 150-line libudev for FFmpeg's V4L2 request hwaccel: it answers the few calls the hwaccel makes
# to find the decoder (enumerate the "media" subsystem, map a device number to /dev/media* and
# /dev/video*) by looking at devfs. Installed in its own directory so that it never shadows
# devel/libudev-devd (which is what other programs want).
#
# DISTFILES: the freebsd-media repository (github.com/bzdOS/freebsd-media) is private at the time of
# writing; set GH_TAGNAME to a release tag once there is one, then "make makesum". distinfo is a
# placeholder until then.

PORTNAME=	libudev-bzdos
DISTVERSION=	0.1
CATEGORIES=	devel

MAINTAINER=	ap.bodrov@gmail.com
COMMENT=	Minimal libudev over devfs for the V4L2 request hwaccel
WWW=		https://github.com/bzdOS/freebsd-media

LICENSE=	BSD2CLAUSE

# libudev.h comes from devel/libudev-devd (the shim implements the same API subset)
BUILD_DEPENDS=	${LOCALBASE}/include/libudev.h:devel/libudev-devd

USE_GITHUB=	yes
GH_ACCOUNT=	bzdOS
GH_PROJECT=	freebsd-media
GH_TAGNAME=	v${DISTVERSION}

UDEVDIR=	${PREFIX}/lib/libudev-bzdos
PLIST_SUB=	UDEVDIR=lib/libudev-bzdos

do-build:
	${CC} ${CFLAGS} -I${LOCALBASE}/include -shared -fPIC -Wl,-soname,libudev.so.1 \
	    -o ${WRKSRC}/libudev.so.1 \
	    ${WRKSRC}/sys/dev/media/tools/ffmpeg/libudev-shim.c
	${PRINTF} 'Name: libudev\nDescription: devfs shim\nVersion: 250\nLibs: -L${UDEVDIR} -ludev\nCflags: -I${LOCALBASE}/include\n' \
	    > ${WRKSRC}/libudev.pc

do-install:
	${MKDIR} ${STAGEDIR}${UDEVDIR}
	${INSTALL_LIB} ${WRKSRC}/libudev.so.1 ${STAGEDIR}${UDEVDIR}/
	${LN} -sf libudev.so.1 ${STAGEDIR}${UDEVDIR}/libudev.so
	${INSTALL_DATA} ${WRKSRC}/libudev.pc ${STAGEDIR}${UDEVDIR}/

.include <bsd.port.mk>
