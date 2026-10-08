#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Construct flat H.264 probes with one macroblock operation per picture.

This is a bitstream fixture generator; it never reconstructs compressed pixels.
JM and FFmpeg are external test oracles only.
"""
from pathlib import Path
import json

B = Path(__file__).resolve().parent
W, H = 176, 144
COUNT = W // 16 * (H // 16)


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, value, width=1):
        self.bits.extend((value >> i) & 1 for i in range(width - 1, -1, -1))

    def ue(self, value):
        width = (value + 1).bit_length()
        self.u(0, width - 1)
        self.u(value + 1, width)

    def se(self, value):
        self.ue(-2 * value if value <= 0 else 2 * value - 1)

    def align(self):
        while len(self.bits) % 8:
            self.u(0)

    def tail(self):
        self.u(1)
        self.align()

    def data(self):
        assert not len(self.bits) % 8
        return bytes(sum(self.bits[i + j] << (7 - j) for j in range(8))
                     for i in range(0, len(self.bits), 8))


def nal(kind, payload):
    out = bytearray(b'\0\0\0\1' + bytes([kind]))
    zeros = 0
    for value in payload:
        if zeros == 2 and value <= 3:
            out.append(3)
            zeros = 0
        out.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return bytes(out)


def parameters():
    s = Bits()
    s.u(88, 8); s.u(0, 8); s.u(30, 8)
    s.ue(0); s.ue(0); s.ue(2); s.ue(1); s.u(0)
    s.ue(W // 16 - 1); s.ue(H // 16 - 1)
    s.u(1); s.u(1); s.u(0); s.u(0); s.tail()
    p = Bits()
    p.ue(0); p.ue(0); p.u(0); p.u(0); p.ue(0)
    p.ue(0); p.ue(0); p.u(0); p.u(0, 2)
    p.se(0); p.se(0); p.se(0); p.u(1); p.u(0); p.u(0); p.tail()
    return nal(0x67, s.data()) + nal(0x68, p.data())


def picture(kind, operation, qs=26, switch=0, idr=False):
    s = Bits()
    s.ue(0); s.ue(kind); s.ue(0); s.u(0 if idr else 1, 4)
    if idr:
        s.ue(0)
    if kind in (0, 3):
        s.u(0); s.u(0)  # one active reference, no list modification
    if idr:
        s.u(0); s.u(0)
    else:
        s.u(0)
    s.se(0)  # QP=26
    if kind == 3:
        s.u(switch); s.se(qs - 26)
    s.ue(1)  # deblocking disabled
    header_bits = len(s.bits) + 40
    if operation == 'skip':
        assert kind in (0, 3)
        s.ue(COUNT)
    else:
        for _ in range(COUNT):
            if kind in (0, 3):
                s.ue(0)  # mb_skip_run
            if operation == 'pcm':
                s.ue(30 if kind in (0, 3) else 25)
                s.align()
                for _ in range(384):
                    s.u(128, 8)
            elif operation == 'inter-zero':
                assert kind in (0, 3)
                s.ue(0)  # P_L0_16x16; ref_idx omitted for one reference
                s.se(0); s.se(0)  # zero motion vector difference
                s.ue(0)  # inter CBP=0
            elif operation == 'intra16-zero':
                # I16x16 DC, luma/chroma CBP=0. DC coeff_token TotalCoeff=0.
                s.ue(8 if kind in (0, 3) else 3)
                s.ue(0); s.se(0); s.u(1)
            else:
                raise ValueError(operation)
    s.tail()
    return nal(0x65 if idr else 0x41, s.data()), header_bits


idr, _ = picture(2, 'pcm', idr=True)
manifest = []
for operation in ('pcm', 'skip', 'inter-zero', 'intra16-zero'):
    for name, kind, qs, switch in [('p', 0, 26, 0), ('sp26', 3, 26, 0),
                                  ('sp36', 3, 36, 0), ('sp36-switch', 3, 36, 1)]:
        if operation == 'pcm' and name == 'p':
            continue  # preceding I_PCM plus other P controls cover transport
        case = f'{name}-{operation}'
        d = B / case
        d.mkdir(exist_ok=True)
        payload, header = picture(kind, operation, qs, switch)
        stream = parameters() + idr + payload
        (d / 'stream.264').write_bytes(stream)
        (d / 'slice-0.bin').write_bytes(idr)
        (d / 'slice-1.bin').write_bytes(payload)
        params = dict(name=case, width=W, height=H, slice_type=kind,
                      operation=operation, qs=qs, switch=switch,
                      header_bits=header, bytes=len(payload))
        (d / 'params.json').write_text(json.dumps(params, indent=2) + '\n')
        manifest.append(params)
(B / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(f'{len(manifest)} fixtures, {COUNT} macroblocks per picture')
