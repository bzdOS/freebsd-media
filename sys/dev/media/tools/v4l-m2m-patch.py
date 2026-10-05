#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Make a FreeBSD v4l-utils binary recognise mem2mem devices (a test-only hack).

On a system that is not Linux, v4l-utils decides what kind of device a node is
by asking VIDIOC_QUERYCAP and looking at device_caps (common/media-info.cpp,
mi_media_detect_type): V4L2_CAP_VIDEO_CAPTURE is a video device, VBI_CAPTURE a
VBI one, RADIO a radio, and anything else is "unable to detect what device this
is". A mem2mem device only has V4L2_CAP_VIDEO_M2M, and an output-only one
only OUTPUT, so neither v4l2-ctl nor v4l2-compliance will open them.

The proper fix is a patch to that function (test the M2M/OUTPUT/META bits too).
Until the port carries it, this patches the compiled aarch64 code: in the
sequence
    tbnz w8, #0, <video> ; tbnz w8, #4, <vbi> ; tbz w8, #18, <unknown> ...
the VBI test (bit 4) becomes a second VIDEO test on bit 15 (V4L2_CAP_VIDEO_M2M).
Capture devices are still found by the first test; VBI devices are no longer
recognised, which nothing here needs.

    v4l-m2m-patch.py <in> <out>
"""
import sys

PATTERN = bytes.fromhex('a8000037' 'c8002037')     # tbnz w8,#0 ; tbnz w8,#4
FOLLOWER = bytes.fromhex('08fe9736')               # tbz  w8,#18 (RADIO)
REPLACEMENT = bytes.fromhex('88007837')            # tbnz w8,#15, same target


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], 'rb').read())
    hits = [i for i in range(len(data) - 12) if data[i:i + 8] == PATTERN
            and data[i + 8:i + 12] == FOLLOWER]
    if len(hits) != 1:
        sys.exit('%s: expected one device-detection sequence, found %d (a different '
                 'build?)' % (sys.argv[1], len(hits)))
    data[hits[0] + 4:hits[0] + 8] = REPLACEMENT
    open(sys.argv[2], 'wb').write(data)
    print('patched %s at file offset %#x' % (sys.argv[2], hits[0] + 4))


if __name__ == '__main__':
    main()
