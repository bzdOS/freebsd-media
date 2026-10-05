/* SPDX-License-Identifier: BSD-2-Clause */
/* Calls between the media core's own files; drivers do not include this. */
#ifndef _DEV_MEDIA_MEDIA_PRIV_H_
#define	_DEV_MEDIA_MEDIA_PRIV_H_

#include <dev/media/media.h>

/* ---- vbq.c: called by vdev.c with v->lock held, except dqbuf/poll/kqfilter */
int  vbq_reqbufs(struct vbq *, struct vfh *, struct v4l2_requestbuffers *);
int  vbq_querybuf(struct vbq *, struct v4l2_buffer *);
int  vbq_qbuf(struct vbq *, struct v4l2_buffer *);
int  vbq_prepare_buf(struct vbq *, struct v4l2_buffer *);
int  vbq_dqbuf(struct vbq *, struct v4l2_buffer *, bool nonblock);
int  vbq_streamon(struct vbq *, enum v4l2_buf_type);
int  vbq_streamoff(struct vbq *, enum v4l2_buf_type);
int  vbq_mmap_single(struct vbq *, vm_ooffset_t *, vm_size_t, vm_object_t *, int);
int  vbq_poll(struct vbq *, int events, struct thread *);
int  vbq_kqfilter(struct vbq *, struct knote *);
/* The fh is going away: drop what it owns. */
void vbq_release(struct vbq *, struct vfh *);
/* The device is going away: stop and free everything, whoever owns it. */
void vbq_shutdown(struct vbq *);
bool vbq_busy_for(struct vbq *, struct vfh *);	/* owned by another fh */
bool vbq_has_buffers(struct vbq *);

/* ---- vctrl.c */
int  vctrl_ioctl_queryctrl(struct vctrl_handler *, struct v4l2_queryctrl *);
int  vctrl_ioctl_query_ext_ctrl(struct vctrl_handler *, struct v4l2_query_ext_ctrl *);
int  vctrl_ioctl_querymenu(struct vctrl_handler *, struct v4l2_querymenu *);
int  vctrl_ioctl_g_ctrl(struct vctrl_handler *, struct v4l2_control *);
int  vctrl_ioctl_s_ctrl(struct vdev *, struct v4l2_control *);
int  vctrl_ioctl_ext_ctrls(struct vdev *, u_long cmd, struct v4l2_ext_controls *);

#endif
