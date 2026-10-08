/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The userland ABI of a dma-buf file descriptor that our native dmabuf (sys/dev/dmabuf) honours: the
 * DMA_BUF_IOCTL_SYNC bracket around CPU access (the same ioctl number and structure as Linux's uapi, so
 * unmodified userland such as FFmpeg's DRM frame mapping works). Written from the published interface.
 */
#ifndef _UAPI_LINUX_DMA_BUF_H
#define _UAPI_LINUX_DMA_BUF_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_SYNC_READ	(1 << 0)
#define DMA_BUF_SYNC_WRITE	(2 << 0)
#define DMA_BUF_SYNC_RW		(DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START	(0 << 2)
#define DMA_BUF_SYNC_END	(1 << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK	(DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)

#define DMA_BUF_BASE		'b'
#define DMA_BUF_IOCTL_SYNC	_IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

#endif
