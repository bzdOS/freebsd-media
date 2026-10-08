# UNTESTED: written on a Linux host without a ports tree or poudriere. Run "portlint -A",
# "make makesum" (the distfile NAME in distinfo is my reading of the USE_GITHUB naming rule; the SHA256
# and SIZE are real, measured on the GitHub tarball of the pinned commit) and a poudriere build
# before relying on it.
#
# FFmpeg 8.1 from Kwiboo's v4l2-request-n8.1 branch: the only FFmpeg with a stateless V4L2
# hwaccel (FFmpeg's own releases up to 9.0.2 have none). It decodes through freebsd-media's
# /dev/video* and /dev/media* (cedrus(4)), -hwaccel v4l2request. It is installed under its own
# prefix so that it never replaces multimedia/ffmpeg.

PORTNAME=	ffmpeg-v4l2request
DISTVERSION=	8.1
CATEGORIES=	multimedia

MAINTAINER=	ap.bodrov@gmail.com
COMMENT=	FFmpeg with the stateless V4L2 request hwaccel (Kwiboo branch)
WWW=		https://github.com/Kwiboo/FFmpeg/tree/v4l2-request-n8.1

LICENSE=	LGPL21+
LICENSE_FILE=	${WRKSRC}/COPYING.LGPLv2.1

ONLY_FOR_ARCHS=	aarch64
ONLY_FOR_ARCHS_REASON=	only the Allwinner A64 video engine (cedrus(4)) has a driver

BUILD_DEPENDS=	${LOCALBASE}/include/v4l2-uapi-bzdos/linux/videodev2.h:devel/v4l2-uapi-bzdos
LIB_DEPENDS=	libdrm.so:graphics/libdrm \
		libudev.so.1:devel/libudev-bzdos

USES=		gmake pkgconfig
USE_GITHUB=	yes
GH_ACCOUNT=	Kwiboo
GH_PROJECT=	FFmpeg
GH_TAGNAME=	b57fbbe50c9b2656fad86a1a7eeabfd2b2a50935

HAS_CONFIGURE=	yes
FFPREFIX=	${PREFIX}/ffmpeg-v4l2request
UDEVDIR=	${LOCALBASE}/lib/libudev-bzdos
V4L2INC=	${LOCALBASE}/include/v4l2-uapi-bzdos

CONFIGURE_ENV=	PKG_CONFIG_PATH=${UDEVDIR}
# The flags are those of sys/dev/media/tools/ffmpeg/build-ffmpeg.sh, the tested cross build, minus
# --enable-cross-compile and the sysroot.
CONFIGURE_ARGS=	--prefix=${FFPREFIX} \
		--cc="${CC}" --disable-everything --disable-doc --disable-debug \
		--disable-autodetect --disable-network \
		--enable-libdrm --enable-libudev --enable-v4l2-request \
		--extra-cflags="-I${LOCALBASE}/include -I${V4L2INC}" \
		--extra-ldflags="-L${LOCALBASE}/lib -L${UDEVDIR} -Wl,-rpath,${UDEVDIR}" \
		--enable-decoder=h264,mpeg2video,rawvideo \
		--enable-parser=h264,mpegvideo \
		--enable-demuxer=h264,mpegvideo,mov,matroska,mpegts,rawvideo \
		--enable-muxer=rawvideo,null,mp4,framemd5,md5 \
		--enable-encoder=rawvideo,wrapped_avframe \
		--enable-protocol=file,pipe --enable-bsf=h264_mp4toannexb,null \
		--enable-filter=scale,format,null,hwdownload,hwmap,copy \
		--enable-swscale \
		--enable-hwaccel=h264_v4l2request,mpeg2_v4l2request

# pkg-plist: generate it on the first build ("make makeplist > pkg-plist": the shared library
# versions are only known after a build). Everything is below ${FFPREFIX}.
# The kernel modules (dmabuf_base, media, cedrus) are not a port: see docs/INSTALL.md.

.include <bsd.port.mk>
