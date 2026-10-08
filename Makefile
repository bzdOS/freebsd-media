# SPDX-License-Identifier: BSD-2-Clause
#
# Top-level Makefile of freebsd-media.
#
# Native build on FreeBSD (the running kernel's sources or headers in /usr/src/sys, or SYSDIR=...):
#
#	make                      # dmabuf_base.ko, media.ko, cedrus.ko   (WITH_VTEST=1 adds vtest.ko)
#	make install              # into ${DESTDIR}/boot/modules, runs kldxref
#	make install-man          # man pages into ${DESTDIR}${PREFIX}/share/man
#	make install-conf         # loader.conf.d, rc.conf.d, devfs.rules samples (see docs/INSTALL.md)
#
# Cross-build on a Linux host into a staging root for aarch64 (needs bmake, clang, lld and a FreeBSD
# source tree of the board's kernel, see docs/INSTALL.md):
#
#	make stage DESTDIR=/path/to/stage [FBSD_SRC=/path/to/freebsd-src] [OBJ=/path/to/objdir]
#
# The modules must be built against the headers of the kernel they will run on.

PREFIX?=	/usr/local
EXAMPLES=	${PREFIX}/share/examples/freebsd-media

# Order matters for a serial build: media depends on dmabuf_base, cedrus on media.
SUBDIR=		sys/modules/dmabuf sys/modules/media sys/modules/cedrus
.if defined(WITH_VTEST)
SUBDIR+=	sys/modules/vtest
.endif

MANPAGES=	share/man/man4/media.4 share/man/man4/cedrus.4 share/man/man4/dmabuf.4 \
		share/man/man7/v4l2_stateless.7

.include <bsd.subdir.mk>

install-man:
.for m in ${MANPAGES}
	${INSTALL} -d ${DESTDIR}${PREFIX}/share/man/man${m:E}
	${INSTALL} -m 444 ${.CURDIR}/${m} ${DESTDIR}${PREFIX}/share/man/man${m:E}/
.endfor

# rc.conf.d/kld and devfs.rules are samples: merging them into the live files is the owner's step.
install-conf:
	${INSTALL} -d ${DESTDIR}/boot/loader.conf.d
	${INSTALL} -m 644 ${.CURDIR}/etc/loader.conf.d/v4l2-cedrus.conf ${DESTDIR}/boot/loader.conf.d/
	${INSTALL} -d ${DESTDIR}${EXAMPLES}
	${INSTALL} -m 644 ${.CURDIR}/etc/rc.conf.d/kld ${.CURDIR}/etc/devfs.rules ${DESTDIR}${EXAMPLES}/

# Cross-build and install into a staging root (see sys/dev/media/tools/stage.sh).
stage:
	@test -n "${DESTDIR}" || { echo "stage: set DESTDIR=<staging root>"; exit 2; }
	DESTDIR=${DESTDIR} PREFIX=${PREFIX} ${.CURDIR}/sys/dev/media/tools/stage.sh

.PHONY: install-man install-conf stage
