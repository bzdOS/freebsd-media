/* SPDX-License-Identifier: BSD-2-Clause */
/* The bare aarch64-linux-gnu cross compiler has no libc headers; videodev2.h
 * needs struct timeval and struct timespec, and on LP64 Linux both are two longs. */
#ifndef _MEDIA_A64_SHIM_SYS_TIME_H_
#define	_MEDIA_A64_SHIM_SYS_TIME_H_
struct timeval { long tv_sec; long tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };
#endif
