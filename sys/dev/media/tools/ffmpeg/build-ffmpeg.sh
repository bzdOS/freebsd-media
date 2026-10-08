#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# build-ffmpeg.sh fetch|stage|configure|make|player|push|all — FFmpeg with the V4L2 Request hwaccel for the board,
# cross-built on the build host (a native build on the guest crashed gmake and is not worth it: 850 MB).
#
# FFmpeg's releases (up to 9.0.2) have no stateless V4L2 hwaccel; Kwiboo's branch v4l2-request-n8.1 has it.
# Two things FreeBSD lacks are provided here: libudev (libudev-shim.c, over devfs) and the linux/ headers
# (this repository's sys/dev/media/{shim,uapi}). libdrm's headers and library come from the guest (stage).
#
#   fetch      download the branch to $W
#   stage      libdrm from the guest, the udev shim, pkg-config files        ($W/stage)
#   configure  cross-configure into $W/xbuild
#   make       build and install into $W/install (needs the three steps above)
#   player     cross-compile fbplay.c and ffkms.c against the installed libraries -> $W/fbplay, $W/ffkms
#   push       copy the result to /var/tmp/ffv4l2 on the guest (the SD card; /tmp is tmpfs and wiped)
#   all        everything in turn
# Test: ffmpeg-test.sh (and corpus-sweep.py for the JVT corpus).
set -eu
W=${W:-/opt/bzdos/linux-work/ffmpeg}
SRC=$W/FFmpeg-4l2-request-n8.1
SR=${SYSROOT:-/opt/bzdos/sysroots/aarch64-15.1}
K=/root/.ssh/chimp_ed25519
G=root@192.0.2.82
here=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$here/../.." && pwd)		# sys/dev/media
S=$W/stage
step=${1:-}

do_fetch() {
	mkdir -p $W && cd $W
	env -u http_proxy -u https_proxy curl -sL -o kwiboo-n8.1.tar.gz https://github.com/Kwiboo/FFmpeg/archive/refs/heads/v4l2-request-n8.1.tar.gz
	tar xzf kwiboo-n8.1.tar.gz
}
do_stage() {
	rm -rf $S; mkdir -p $S/include $S/lib/pkgconfig
	scp -q -r -i $K $G:/usr/local/include/libdrm $S/include/
	scp -q -i $K $G:/usr/local/include/xf86drm.h $G:/usr/local/include/xf86drmMode.h $G:/usr/local/include/libudev.h $S/include/
	scp -q -i $K "$G:/usr/local/lib/libdrm.so*" $S/lib/
	clang --target=aarch64-unknown-freebsd15.1 --sysroot=$SR -O2 -shared -fPIC -fuse-ld=lld -Wl,-soname,libudev.so.1 \
	    -o $S/lib/libudev.so.1 $here/libudev-shim.c
	ln -sf libudev.so.1 $S/lib/libudev.so
	printf 'Name: libudev\nDescription: FreeBSD devfs shim\nVersion: 250\nLibs: -L%s/lib -ludev\nCflags: -I%s/include\n' $S $S > $S/lib/pkgconfig/libudev.pc
	printf 'Name: libdrm\nDescription: libdrm\nVersion: 2.4.133\nLibs: -L%s/lib -ldrm\nCflags: -I%s/include -I%s/include/libdrm\n' $S $S $S > $S/lib/pkgconfig/libdrm.pc
}
do_configure() {
	rm -rf $W/xbuild; mkdir -p $W/xbuild; cd $W/xbuild
	PKG_CONFIG_LIBDIR=$S/lib/pkgconfig PKG_CONFIG_PATH= $SRC/configure --prefix=/var/tmp/ffv4l2 \
	    --enable-cross-compile --arch=aarch64 --target-os=freebsd \
	    --cc="clang --target=aarch64-unknown-freebsd15.1 --sysroot=$SR" \
	    --ld="clang --target=aarch64-unknown-freebsd15.1 --sysroot=$SR -fuse-ld=lld" \
	    --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib --strip=llvm-strip --pkg-config=pkg-config \
	    --disable-everything --disable-doc --disable-debug --disable-autodetect --disable-network \
	    --enable-libdrm --enable-libudev --enable-v4l2-request \
	    --extra-cflags="-I$M/shim -I$M/uapi" --extra-ldflags="-Wl,-rpath,/var/tmp/ffv4l2/udev/lib -L$S/lib" \
	    --enable-decoder=h264,mpeg2video,rawvideo --enable-parser=h264,mpegvideo \
	    --enable-decoder=aac,aac_latm,mp3float,mp2float,ac3,flac,vorbis,pcm_s16le,pcm_s16be,pcm_s24le \
	    --enable-parser=aac,aac_latm,mpegaudio,ac3,flac,vorbis --enable-swresample \
	    --enable-demuxer=aac,mp3,wav,ac3,flac,ogg,mpegps,avi \
	    --enable-demuxer=h264,mpegvideo,mov,matroska,mpegts,rawvideo --enable-muxer=rawvideo,null,mp4,framemd5,md5 \
	    --enable-encoder=rawvideo,wrapped_avframe --enable-protocol=file,pipe --enable-bsf=h264_mp4toannexb,null \
	    --enable-filter=scale,format,null,hwdownload,hwmap,copy --enable-swscale \
	    --enable-hwaccel=h264_v4l2request,mpeg2_v4l2request > configure.log 2>&1
}
do_player() {		# fbplay (and ffkms): linked statically against the FFmpeg libraries in $W/install
	I=$W/install/var/tmp/ffv4l2
	for p in fbplay ffkms; do
		clang --target=aarch64-unknown-freebsd15.1 --sysroot=$SR -O2 -Wall -fuse-ld=lld -I$I/include -I$S/include \
		    -I$S/include/libdrm -o $W/$p $here/$p.c -L$I/lib -L$S/lib -Wl,-rpath,/var/tmp/ffv4l2/udev/lib \
		    -Wl,-rpath,/usr/local/lib -lavformat -lavcodec -lswresample -lswscale -lavutil -ldrm -ludev -lEGL -lgbm \
		    -lGLESv2 -lm -lpthread
	done
}
do_make() {
	cd $W/xbuild && make -j"$(nproc)" > make.log 2>&1
	rm -rf $W/install; make install DESTDIR=$W/install > /dev/null 2>&1
}
do_push() {
	ssh -i $K $G 'mkdir -p /var/tmp/ffv4l2/udev/lib'
	tar -C $W/install/var/tmp/ffv4l2 -czf - bin lib | ssh -i $K $G 'tar -C /var/tmp/ffv4l2 -xzf -'
	[ -x $W/fbplay ] && scp -q -i $K $W/fbplay $W/ffkms $G:/var/tmp/ffv4l2/bin/
	scp -q -i $K $S/lib/libudev.so.1 $G:/var/tmp/ffv4l2/udev/lib/
	ssh -i $K $G 'LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib /var/tmp/ffv4l2/bin/ffmpeg -hwaccels 2>&1 | tail -3'
}
case "$step" in
fetch) do_fetch ;; stage) do_stage ;; configure) do_configure ;; make) do_make ;; player) do_player ;; push) do_push ;;
all) do_fetch; do_stage; do_configure; do_make; do_player; do_push ;;
*) echo "usage: $0 fetch|stage|configure|make|player|push|all" >&2; exit 2 ;;
esac
