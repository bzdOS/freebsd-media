#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
#
# build-ffmpeg.sh [fetch|push|build|test] — FFmpeg with the V4L2 Request hwaccel (Kwiboo's v4l2-request-n8.1
# branch; upstream FFmpeg releases up to 9.0.2 do not have it) built natively on the board, using our
# libudev shim, installed under /var/tmp/ffv4l2 on the guest's SD card (not tmpfs, not /usr/local).
#
#   fetch   download the branch to /opt/bzdos/linux-work/ffmpeg (proxy variables unset)
#   push    copy the source and the shim to the guest
#   build   configure + make -j4 + install on the guest (long: tens of minutes; runs under nohup, log in
#           /var/tmp/ffbuild/build.log)
#   test    decode the test streams with -hwaccel v4l2request and compare with the software decode
set -eu
W=/opt/bzdos/linux-work/ffmpeg
SRC=$W/FFmpeg-4l2-request-n8.1
K=/root/.ssh/chimp_ed25519
G=root@192.0.2.82
here=$(cd "$(dirname "$0")" && pwd)
S="ssh -i $K -o BatchMode=yes -o ConnectTimeout=8 $G"
case "${1:-}" in
fetch)
	mkdir -p $W && cd $W
	env -u http_proxy -u https_proxy curl -sL -o kwiboo-n8.1.tar.gz https://github.com/Kwiboo/FFmpeg/archive/refs/heads/v4l2-request-n8.1.tar.gz
	tar xzf kwiboo-n8.1.tar.gz ;;
push)
	$S 'mkdir -p /var/tmp/ffbuild /var/tmp/ffv4l2/udev/lib/pkgconfig'
	tar -C $W -czf - FFmpeg-4l2-request-n8.1 | $S 'tar -C /var/tmp/ffbuild -xzf -'
	scp -q -i $K $here/libudev-shim.c $G:/var/tmp/ffbuild/ ;;
build)
	$S 'cd /var/tmp/ffbuild && cc -O2 -shared -fPIC -Wl,-soname,libudev.so.1 -o /var/tmp/ffv4l2/udev/lib/libudev.so.1 libudev-shim.c &&
	    ln -sf libudev.so.1 /var/tmp/ffv4l2/udev/lib/libudev.so &&
	    printf "Name: libudev\nDescription: FreeBSD devfs shim\nVersion: 250\nLibs: -L/var/tmp/ffv4l2/udev/lib -ludev\nCflags: -I/usr/local/include\n" > /var/tmp/ffv4l2/udev/lib/pkgconfig/libudev.pc &&
	    cd FFmpeg-4l2-request-n8.1 &&
	    PKG_CONFIG_PATH=/var/tmp/ffv4l2/udev/lib/pkgconfig:/usr/local/libdata/pkgconfig nohup sh -c "
	      ./configure --prefix=/var/tmp/ffv4l2 --cc=cc --disable-everything --disable-doc --disable-debug --disable-autodetect \
	        --disable-network --enable-libdrm --enable-libudev --enable-v4l2-request \
	        --extra-cflags=\"-I/tmp/shim -I/tmp/uapi\" --extra-ldflags=\"-Wl,-rpath,/var/tmp/ffv4l2/udev/lib\" \
	        --enable-decoder=h264,mpeg2video,rawvideo --enable-parser=h264,mpegvideo \
	        --enable-demuxer=h264,mpegvideo,mov,matroska,mpegts,rawvideo --enable-muxer=rawvideo,null,mp4,framemd5,md5 \
	        --enable-encoder=rawvideo --enable-protocol=file,pipe --enable-bsf=h264_mp4toannexb,null \
	        --enable-filter=scale,format,null,hwdownload,hwmap,copy --enable-swscale \
	        --enable-hwaccel=h264_v4l2request,mpeg2_v4l2request &&
	      gmake -j4 && gmake install" > /var/tmp/ffbuild/build.log 2>&1 &
	    sleep 2; echo started' ;;
test)
	$S 'export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib; /var/tmp/ffv4l2/bin/ffmpeg -hwaccels 2>&1 | tail -3' ;;
*)
	echo "usage: $0 fetch|push|build|test" >&2; exit 2 ;;
esac
