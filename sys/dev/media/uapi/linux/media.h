/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * GENERATED FILE - do not edit.
 * Source: sys/dev/media/uapi/media.spec
 * Generator: sys/dev/media/tools/gen-media-h.py
 *
 * Media controller userspace interface: constants, structures and ioctls.
 */
#ifndef _UAPI_LINUX_MEDIA_H
#define _UAPI_LINUX_MEDIA_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Entity functions: what an entity does. */
#define MEDIA_ENT_F_BASE			0x0
#define MEDIA_ENT_F_UNKNOWN			0x0
#define MEDIA_ENT_F_DTV_DEMOD			0x1
#define MEDIA_ENT_F_TS_DEMUX			0x2
#define MEDIA_ENT_F_DTV_CA			0x3
#define MEDIA_ENT_F_DTV_NET_DECAP		0x4
#define MEDIA_ENT_F_IO_DTV			0x1001
#define MEDIA_ENT_F_IO_VBI			0x1002
#define MEDIA_ENT_F_IO_SWRADIO			0x1003
#define MEDIA_ENT_F_IF_VID_DECODER		0x2001
#define MEDIA_ENT_F_IF_AUD_DECODER		0x2002
#define MEDIA_ENT_F_AUDIO_CAPTURE		0x3001
#define MEDIA_ENT_F_AUDIO_PLAYBACK		0x3002
#define MEDIA_ENT_F_AUDIO_MIXER			0x3003
#define MEDIA_ENT_F_PROC_VIDEO_COMPOSER		0x4001
#define MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER	0x4002
#define MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV	0x4003
#define MEDIA_ENT_F_PROC_VIDEO_LUT		0x4004
#define MEDIA_ENT_F_PROC_VIDEO_SCALER		0x4005
#define MEDIA_ENT_F_PROC_VIDEO_STATISTICS	0x4006
#define MEDIA_ENT_F_PROC_VIDEO_ENCODER		0x4007
#define MEDIA_ENT_F_PROC_VIDEO_DECODER		0x4008
#define MEDIA_ENT_F_PROC_VIDEO_ISP		0x4009
#define MEDIA_ENT_F_VID_MUX			0x5001
#define MEDIA_ENT_F_VID_IF_BRIDGE		0x5002
#define MEDIA_ENT_F_DTV_DECODER			0x6001
#define MEDIA_ENT_F_DV_DECODER			0x6001
#define MEDIA_ENT_F_DV_ENCODER			0x6002
#define MEDIA_ENT_F_OLD_BASE			0x10000
#define MEDIA_ENT_F_IO_V4L			0x10001
#define MEDIA_ENT_F_OLD_SUBDEV_BASE		0x20000
#define MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN		0x20000
#define MEDIA_ENT_F_CAM_SENSOR			0x20001
#define MEDIA_ENT_F_FLASH			0x20002
#define MEDIA_ENT_F_LENS			0x20003
#define MEDIA_ENT_F_ATV_DECODER			0x20004
#define MEDIA_ENT_F_TUNER			0x20005

/* Legacy entity types, kept for old callers. */
#define MEDIA_ENT_T_UNKNOWN			0x0
#define MEDIA_ENT_T_DEVNODE			0x10000
#define MEDIA_ENT_T_DEVNODE_V4L			0x10001
#define MEDIA_ENT_T_V4L2_VIDEO			0x10001
#define MEDIA_ENT_T_DEVNODE_FB			0x10002
#define MEDIA_ENT_T_DEVNODE_ALSA		0x10003
#define MEDIA_ENT_T_DEVNODE_DVB			0x10004
#define MEDIA_ENT_T_DEVNODE_UNKNOWN		0x1ffff
#define MEDIA_ENT_T_V4L2_SUBDEV			0x20000
#define MEDIA_ENT_T_V4L2_SUBDEV_SENSOR		0x20001
#define MEDIA_ENT_T_V4L2_SUBDEV_FLASH		0x20002
#define MEDIA_ENT_T_V4L2_SUBDEV_LENS		0x20003
#define MEDIA_ENT_T_V4L2_SUBDEV_DECODER		0x20004
#define MEDIA_ENT_T_V4L2_SUBDEV_TUNER		0x20005

/* Entity id flag, type/subtype masks and entity flags. */
#define MEDIA_ENT_FL_DEFAULT	0x1
#define MEDIA_ENT_FL_CONNECTOR	0x2
#define MEDIA_ENT_TYPE_SHIFT	0x10
#define MEDIA_ENT_SUBTYPE_MASK	0xffff
#define MEDIA_ENT_TYPE_MASK	0xff0000
#define MEDIA_ENT_ID_FLAG_NEXT	0x80000000

/* Interface (device node) types. */
#define MEDIA_INTF_T_DVB_BASE		0x100
#define MEDIA_INTF_T_DVB_FE		0x100
#define MEDIA_INTF_T_DVB_DEMUX		0x101
#define MEDIA_INTF_T_DVB_DVR		0x102
#define MEDIA_INTF_T_DVB_CA		0x103
#define MEDIA_INTF_T_DVB_NET		0x104
#define MEDIA_INTF_T_V4L_BASE		0x200
#define MEDIA_INTF_T_V4L_VIDEO		0x200
#define MEDIA_INTF_T_V4L_VBI		0x201
#define MEDIA_INTF_T_V4L_RADIO		0x202
#define MEDIA_INTF_T_V4L_SUBDEV		0x203
#define MEDIA_INTF_T_V4L_SWRADIO	0x204
#define MEDIA_INTF_T_V4L_TOUCH		0x205
#define MEDIA_INTF_T_ALSA_BASE		0x300
#define MEDIA_INTF_T_ALSA_PCM_CAPTURE	0x300
#define MEDIA_INTF_T_ALSA_PCM_PLAYBACK	0x301
#define MEDIA_INTF_T_ALSA_CONTROL	0x302
#define MEDIA_INTF_T_ALSA_COMPRESS	0x303
#define MEDIA_INTF_T_ALSA_RAWMIDI	0x304
#define MEDIA_INTF_T_ALSA_HWDEP		0x305
#define MEDIA_INTF_T_ALSA_SEQUENCER	0x306
#define MEDIA_INTF_T_ALSA_TIMER		0x307

/* Link flags. */
#define MEDIA_LNK_FL_DATA_LINK		0x0
#define MEDIA_LNK_FL_ENABLED		0x1
#define MEDIA_LNK_FL_IMMUTABLE		0x2
#define MEDIA_LNK_FL_DYNAMIC		0x4
#define MEDIA_LNK_FL_INTERFACE_LINK	0x10000000
#define MEDIA_LNK_FL_ANCILLARY_LINK	0x20000000
#define MEDIA_LNK_FL_LINK_TYPE		0xf0000000

/* Pad flags. */
#define MEDIA_PAD_FL_SINK		0x1
#define MEDIA_PAD_FL_SOURCE		0x2
#define MEDIA_PAD_FL_MUST_CONNECT	0x4

/* Interface revision. */
#define MEDIA_API_VERSION	0x100

/* Identity of a media device: driver, model, bus and version numbers. */
struct media_device_info {
	char	driver[16];
	char	model[32];
	char	serial[40];
	char	bus_info[32];
	__u32	media_version;
	__u32	hw_revision;
	__u32	driver_version;
	__u32	reserved[31];
};

/* One graph entity as listed by ENUM_ENTITIES, with its node numbers. */
struct media_entity_desc {
	__u32	id;
	char	name[32];
	__u32	type;
	__u32	revision;
	__u32	flags;
	__u32	group_id;
	__u16	pads;
	__u16	links;
	__u32	reserved[4];
	union {
		struct {
			__u32	major;
			__u32	minor;
		} dev;
		struct {
			__u32	card;
			__u32	device;
			__u32	subdevice;
		} alsa;
		struct {
			__u32	major;
			__u32	minor;
		} v4l;
		struct {
			__u32	major;
			__u32	minor;
		} fb;
		int	dvb;
		__u8	raw[184];
	};
};

/* Address of one pad: owning entity, pad index and pad flags. */
struct media_pad_desc {
	__u32	entity;
	__u16	index;
	__u32	flags;
	__u32	reserved[2];
};

/* A link between a source pad and a sink pad, with its flags. */
struct media_link_desc {
	struct media_pad_desc	source;
	struct media_pad_desc	sink;
	__u32			flags;
	__u32			reserved[2];
};

/* Request/reply block for listing the pads and links of an entity. */
struct media_links_enum {
	__u32			entity;
	struct media_pad_desc	*pads;
	struct media_link_desc	*links;
	__u32			reserved[4];
};

/* Topology record for an entity (packed). */
struct media_v2_entity {
	__u32	id;
	char	name[64];
	__u32	function;
	__u32	flags;
	__u32	reserved[5];
} __attribute__((packed));

/* Device node numbers of a topology interface (packed). */
struct media_v2_intf_devnode {
	__u32	major;
	__u32	minor;
} __attribute__((packed));

/* Topology record for a user-visible interface (packed). */
struct media_v2_interface {
	__u32	id;
	__u32	intf_type;
	__u32	flags;
	__u32	reserved[9];
	union {
		struct media_v2_intf_devnode	devnode;
		__u32				raw[16];
	};
} __attribute__((packed));

/* Topology record for a pad (packed). */
struct media_v2_pad {
	__u32	id;
	__u32	entity_id;
	__u32	flags;
	__u32	index;
	__u32	reserved[4];
} __attribute__((packed));

/* Topology record for a link between two ids (packed). */
struct media_v2_link {
	__u32	id;
	__u32	source_id;
	__u32	sink_id;
	__u32	flags;
	__u32	reserved[6];
} __attribute__((packed));

/* Whole-graph snapshot: counts plus user pointers to the record arrays (packed). */
struct media_v2_topology {
	__u64	topology_version;
	__u32	num_entities;
	__u32	reserved1;
	__u64	ptr_entities;
	__u32	num_interfaces;
	__u32	reserved2;
	__u64	ptr_interfaces;
	__u32	num_pads;
	__u32	reserved3;
	__u64	ptr_pads;
	__u32	num_links;
	__u32	reserved4;
	__u64	ptr_links;
} __attribute__((packed));

/*
 * Helper pair for the ioctl numbers below: MEDIA_IOCTL(dir, nr, argtype)
 * selects the _IO/_IOR/_IOW/_IOWR form by token pasting on dir.
 */
#define MEDIA_IOCTL_DIR_N(grp, nr, argt)	_IO(grp, nr)
#define MEDIA_IOCTL_DIR_R(grp, nr, argt)	_IOR(grp, nr, argt)
#define MEDIA_IOCTL_DIR_W(grp, nr, argt)	_IOW(grp, nr, argt)
#define MEDIA_IOCTL_DIR_RW(grp, nr, argt)	_IOWR(grp, nr, argt)
#define MEDIA_IOCTL(dir, nr, argt)	MEDIA_IOCTL_DIR_##dir(0x7c, nr, argt)

#define MEDIA_IOC_DEVICE_INFO		MEDIA_IOCTL(RW, 0x00, struct media_device_info)
#define MEDIA_IOC_ENUM_ENTITIES		MEDIA_IOCTL(RW, 0x01, struct media_entity_desc)
#define MEDIA_IOC_ENUM_LINKS		MEDIA_IOCTL(RW, 0x02, struct media_links_enum)
#define MEDIA_IOC_SETUP_LINK		MEDIA_IOCTL(RW, 0x03, struct media_link_desc)
#define MEDIA_IOC_G_TOPOLOGY		MEDIA_IOCTL(RW, 0x04, struct media_v2_topology)
#define MEDIA_IOC_REQUEST_ALLOC		MEDIA_IOCTL(R, 0x05, int)
#define MEDIA_REQUEST_IOC_QUEUE		MEDIA_IOCTL(N, 0x80, void)
#define MEDIA_REQUEST_IOC_REINIT	MEDIA_IOCTL(N, 0x81, void)

#endif /* _UAPI_LINUX_MEDIA_H */
