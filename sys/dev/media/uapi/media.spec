{
 "comment": "Spec for linux/media.h; read by tools/gen-media-h.py. Facts only.",
 "ioctl_type": 124,
 "family_order": [
  "ent-function",
  "ent-type-legacy",
  "ent-misc",
  "intf-type",
  "link-flag",
  "pad-flag",
  "api"
 ],
 "family_doc": {
  "api": "Interface revision.",
  "ent-function": "Entity functions: what an entity does.",
  "ent-type-legacy": "Legacy entity types, kept for old callers.",
  "ent-misc": "Entity id flag, type/subtype masks and entity flags.",
  "intf-type": "Interface (device node) types.",
  "link-flag": "Link flags.",
  "pad-flag": "Pad flags."
 },
 "structs": [
  {
   "name": "media_device_info",
   "size": 256,
   "doc": "Identity of a media device: driver, model, bus and version numbers.",
   "packed": false,
   "members": [
    {
     "name": "driver",
     "type": "char",
     "count": 16
    },
    {
     "name": "model",
     "type": "char",
     "count": 32
    },
    {
     "name": "serial",
     "type": "char",
     "count": 40
    },
    {
     "name": "bus_info",
     "type": "char",
     "count": 32
    },
    {
     "name": "media_version",
     "type": "__u32"
    },
    {
     "name": "hw_revision",
     "type": "__u32"
    },
    {
     "name": "driver_version",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 31
    }
   ]
  },
  {
   "name": "media_entity_desc",
   "size": 256,
   "doc": "One graph entity as listed by ENUM_ENTITIES, with its node numbers.",
   "packed": false,
   "members": [
    {
     "name": "id",
     "type": "__u32"
    },
    {
     "name": "name",
     "type": "char",
     "count": 32
    },
    {
     "name": "type",
     "type": "__u32"
    },
    {
     "name": "revision",
     "type": "__u32"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "group_id",
     "type": "__u32"
    },
    {
     "name": "pads",
     "type": "__u16"
    },
    {
     "name": "links",
     "type": "__u16"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 4
    }
   ],
   "union": [
    {
     "name": "dev",
     "fields": [
      {
       "name": "major",
       "type": "__u32"
      },
      {
       "name": "minor",
       "type": "__u32"
      }
     ]
    },
    {
     "name": "alsa",
     "fields": [
      {
       "name": "card",
       "type": "__u32"
      },
      {
       "name": "device",
       "type": "__u32"
      },
      {
       "name": "subdevice",
       "type": "__u32"
      }
     ]
    },
    {
     "name": "v4l",
     "fields": [
      {
       "name": "major",
       "type": "__u32"
      },
      {
       "name": "minor",
       "type": "__u32"
      }
     ]
    },
    {
     "name": "fb",
     "fields": [
      {
       "name": "major",
       "type": "__u32"
      },
      {
       "name": "minor",
       "type": "__u32"
      }
     ]
    },
    {
     "name": "dvb",
     "type": "int"
    },
    {
     "name": "raw",
     "type": "__u8",
     "count": 184
    }
   ]
  },
  {
   "name": "media_pad_desc",
   "size": 20,
   "doc": "Address of one pad: owning entity, pad index and pad flags.",
   "packed": false,
   "members": [
    {
     "name": "entity",
     "type": "__u32"
    },
    {
     "name": "index",
     "type": "__u16"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 2
    }
   ]
  },
  {
   "name": "media_link_desc",
   "size": 52,
   "doc": "A link between a source pad and a sink pad, with its flags.",
   "packed": false,
   "members": [
    {
     "name": "source",
     "type": "struct media_pad_desc"
    },
    {
     "name": "sink",
     "type": "struct media_pad_desc"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 2
    }
   ]
  },
  {
   "name": "media_links_enum",
   "size": 40,
   "doc": "Request/reply block for listing the pads and links of an entity.",
   "packed": false,
   "members": [
    {
     "name": "entity",
     "type": "__u32"
    },
    {
     "name": "pads",
     "type": "struct media_pad_desc *"
    },
    {
     "name": "links",
     "type": "struct media_link_desc *"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 4
    }
   ]
  },
  {
   "name": "media_v2_entity",
   "size": 96,
   "doc": "Topology record for an entity (packed).",
   "packed": true,
   "members": [
    {
     "name": "id",
     "type": "__u32"
    },
    {
     "name": "name",
     "type": "char",
     "count": 64
    },
    {
     "name": "function",
     "type": "__u32"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 5
    }
   ]
  },
  {
   "name": "media_v2_intf_devnode",
   "size": 8,
   "doc": "Device node numbers of a topology interface (packed).",
   "packed": true,
   "members": [
    {
     "name": "major",
     "type": "__u32"
    },
    {
     "name": "minor",
     "type": "__u32"
    }
   ]
  },
  {
   "name": "media_v2_interface",
   "size": 112,
   "doc": "Topology record for a user-visible interface (packed).",
   "packed": true,
   "members": [
    {
     "name": "id",
     "type": "__u32"
    },
    {
     "name": "intf_type",
     "type": "__u32"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 9
    }
   ],
   "union": [
    {
     "name": "devnode",
     "type": "struct media_v2_intf_devnode"
    },
    {
     "name": "raw",
     "type": "__u32",
     "count": 16
    }
   ]
  },
  {
   "name": "media_v2_pad",
   "size": 32,
   "doc": "Topology record for a pad (packed).",
   "packed": true,
   "members": [
    {
     "name": "id",
     "type": "__u32"
    },
    {
     "name": "entity_id",
     "type": "__u32"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "index",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 4
    }
   ]
  },
  {
   "name": "media_v2_link",
   "size": 40,
   "doc": "Topology record for a link between two ids (packed).",
   "packed": true,
   "members": [
    {
     "name": "id",
     "type": "__u32"
    },
    {
     "name": "source_id",
     "type": "__u32"
    },
    {
     "name": "sink_id",
     "type": "__u32"
    },
    {
     "name": "flags",
     "type": "__u32"
    },
    {
     "name": "reserved",
     "type": "__u32",
     "count": 6
    }
   ]
  },
  {
   "name": "media_v2_topology",
   "size": 72,
   "doc": "Whole-graph snapshot: counts plus user pointers to the record arrays (packed).",
   "packed": true,
   "members": [
    {
     "name": "topology_version",
     "type": "__u64"
    },
    {
     "name": "num_entities",
     "type": "__u32"
    },
    {
     "name": "reserved1",
     "type": "__u32"
    },
    {
     "name": "ptr_entities",
     "type": "__u64"
    },
    {
     "name": "num_interfaces",
     "type": "__u32"
    },
    {
     "name": "reserved2",
     "type": "__u32"
    },
    {
     "name": "ptr_interfaces",
     "type": "__u64"
    },
    {
     "name": "num_pads",
     "type": "__u32"
    },
    {
     "name": "reserved3",
     "type": "__u32"
    },
    {
     "name": "ptr_pads",
     "type": "__u64"
    },
    {
     "name": "num_links",
     "type": "__u32"
    },
    {
     "name": "reserved4",
     "type": "__u32"
    },
    {
     "name": "ptr_links",
     "type": "__u64"
    }
   ]
  }
 ],
 "constants": [
  {
   "name": "MEDIA_API_VERSION",
   "value": 256,
   "family": "api"
  },
  {
   "name": "MEDIA_ENT_FL_CONNECTOR",
   "value": 2,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_FL_DEFAULT",
   "value": 1,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_F_ATV_DECODER",
   "value": 131076,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_AUDIO_CAPTURE",
   "value": 12289,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_AUDIO_MIXER",
   "value": 12291,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_AUDIO_PLAYBACK",
   "value": 12290,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_BASE",
   "value": 0,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_CAM_SENSOR",
   "value": 131073,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DTV_CA",
   "value": 3,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DTV_DECODER",
   "value": 24577,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DTV_DEMOD",
   "value": 1,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DTV_NET_DECAP",
   "value": 4,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DV_DECODER",
   "value": 24577,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_DV_ENCODER",
   "value": 24578,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_FLASH",
   "value": 131074,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IF_AUD_DECODER",
   "value": 8194,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IF_VID_DECODER",
   "value": 8193,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IO_DTV",
   "value": 4097,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IO_SWRADIO",
   "value": 4099,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IO_V4L",
   "value": 65537,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_IO_VBI",
   "value": 4098,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_LENS",
   "value": 131075,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_OLD_BASE",
   "value": 65536,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_OLD_SUBDEV_BASE",
   "value": 131072,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_COMPOSER",
   "value": 16385,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_DECODER",
   "value": 16392,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_ENCODER",
   "value": 16391,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_ISP",
   "value": 16393,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_LUT",
   "value": 16388,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV",
   "value": 16387,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER",
   "value": 16386,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_SCALER",
   "value": 16389,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_PROC_VIDEO_STATISTICS",
   "value": 16390,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_TS_DEMUX",
   "value": 2,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_TUNER",
   "value": 131077,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_UNKNOWN",
   "value": 0,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN",
   "value": 131072,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_VID_IF_BRIDGE",
   "value": 20482,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_F_VID_MUX",
   "value": 20481,
   "family": "ent-function"
  },
  {
   "name": "MEDIA_ENT_ID_FLAG_NEXT",
   "value": 2147483648,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_SUBTYPE_MASK",
   "value": 65535,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_TYPE_MASK",
   "value": 16711680,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_TYPE_SHIFT",
   "value": 16,
   "family": "ent-misc"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE",
   "value": 65536,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE_ALSA",
   "value": 65539,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE_DVB",
   "value": 65540,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE_FB",
   "value": 65538,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE_UNKNOWN",
   "value": 131071,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_DEVNODE_V4L",
   "value": 65537,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_UNKNOWN",
   "value": 0,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV",
   "value": 131072,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV_DECODER",
   "value": 131076,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV_FLASH",
   "value": 131074,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV_LENS",
   "value": 131075,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV_SENSOR",
   "value": 131073,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_SUBDEV_TUNER",
   "value": 131077,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_ENT_T_V4L2_VIDEO",
   "value": 65537,
   "family": "ent-type-legacy"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_BASE",
   "value": 768,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_COMPRESS",
   "value": 771,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_CONTROL",
   "value": 770,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_HWDEP",
   "value": 773,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_PCM_CAPTURE",
   "value": 768,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_PCM_PLAYBACK",
   "value": 769,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_RAWMIDI",
   "value": 772,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_SEQUENCER",
   "value": 774,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_ALSA_TIMER",
   "value": 775,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_BASE",
   "value": 256,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_CA",
   "value": 259,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_DEMUX",
   "value": 257,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_DVR",
   "value": 258,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_FE",
   "value": 256,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_DVB_NET",
   "value": 260,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_BASE",
   "value": 512,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_RADIO",
   "value": 514,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_SUBDEV",
   "value": 515,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_SWRADIO",
   "value": 516,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_TOUCH",
   "value": 517,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_VBI",
   "value": 513,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_INTF_T_V4L_VIDEO",
   "value": 512,
   "family": "intf-type"
  },
  {
   "name": "MEDIA_LNK_FL_ANCILLARY_LINK",
   "value": 536870912,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_DATA_LINK",
   "value": 0,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_DYNAMIC",
   "value": 4,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_ENABLED",
   "value": 1,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_IMMUTABLE",
   "value": 2,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_INTERFACE_LINK",
   "value": 268435456,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_LNK_FL_LINK_TYPE",
   "value": 4026531840,
   "family": "link-flag"
  },
  {
   "name": "MEDIA_PAD_FL_MUST_CONNECT",
   "value": 4,
   "family": "pad-flag"
  },
  {
   "name": "MEDIA_PAD_FL_SINK",
   "value": 1,
   "family": "pad-flag"
  },
  {
   "name": "MEDIA_PAD_FL_SOURCE",
   "value": 2,
   "family": "pad-flag"
  }
 ],
 "ioctls": [
  {
   "name": "MEDIA_IOC_DEVICE_INFO",
   "dir": "RW",
   "nr": 0,
   "arg": "struct media_device_info"
  },
  {
   "name": "MEDIA_IOC_ENUM_ENTITIES",
   "dir": "RW",
   "nr": 1,
   "arg": "struct media_entity_desc"
  },
  {
   "name": "MEDIA_IOC_ENUM_LINKS",
   "dir": "RW",
   "nr": 2,
   "arg": "struct media_links_enum"
  },
  {
   "name": "MEDIA_IOC_SETUP_LINK",
   "dir": "RW",
   "nr": 3,
   "arg": "struct media_link_desc"
  },
  {
   "name": "MEDIA_IOC_G_TOPOLOGY",
   "dir": "RW",
   "nr": 4,
   "arg": "struct media_v2_topology"
  },
  {
   "name": "MEDIA_IOC_REQUEST_ALLOC",
   "dir": "R",
   "nr": 5,
   "arg": "int"
  },
  {
   "name": "MEDIA_REQUEST_IOC_QUEUE",
   "dir": "N",
   "nr": 128
  },
  {
   "name": "MEDIA_REQUEST_IOC_REINIT",
   "dir": "N",
   "nr": 129
  }
 ]
}