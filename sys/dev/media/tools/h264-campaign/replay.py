#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Replay measured field/frame slices with relocated lab buffers and compare Y/UV."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path('/opt/bzdos')
SSH = ['ssh', '-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes',
       '-o', 'ConnectTimeout=8', 'root@192.0.2.82']
SCP = ['scp', '-q', '-i', '/root/.ssh/chimp_ed25519']


def runs(path):
    result = []
    for line in path.read_text().splitlines():
        row = line.split()
        if row[2:4] != ['W32', 'VE']:
            continue
        off, val = int(row[4], 16), int(row[5], 16)
        if off == 0xec:
            result.append([])
        if result:
            result[-1].extend([(off, val)] * (int(row[6][1:]) if len(row) > 6 else 1))
    return result


def long_term_slots(path):
    """Read the supplied front-end DPB classification, without inferring from POC."""
    result = {}
    picture = None
    for line in path.read_text().splitlines():
        if line.startswith('pic '):
            picture = int(line.split()[1])
            result[picture] = set()
        elif line.startswith('  dpb '):
            match = re.match(r'  dpb (\d+).* fl ([0-9a-f]+)', line)
            if picture is None or match is None:
                raise ValueError('invalid canon DPB record')
            if int(match[2], 16) & 4:
                result[picture].add(int(match[1]))
    return result


def build(pictures, trace, prefix, directory, clear_modes=False, mv_factor=1,
          longterm=None, long_term_mask=0):
    w, h = pictures[0]['width'], pictures[0]['height']
    stride = (w + 15) & ~15
    frame_size = stride * h * 3 // 2
    # Regions never overlap: input [0,1MiB), scratch [1,2MiB),
    # frame/MV allocations [2,8MiB). Each MV allocation has a 4KiB guard.
    cursor = 0x200000
    buffers, motion = {}, {}
    def allocate(mapping, key, size):
        nonlocal cursor
        if key not in mapping:
            mapping[key] = cursor
            cursor += (size + 4095) & ~4095
            if cursor > 0x800000:
                raise ValueError('lab allocation exceeds 8MiB')
        return mapping[key]
    lines = ['reset', 'fill 0 0x800000 0']
    uploads, slices = [], []
    stream_end = 0
    for pic in pictures:
        for index, sl in enumerate(pic['slices']):
            file = prefix.parent / f'{prefix.name}_{pic["picture"]}_{index}.bin'
            if file.stat().st_size != sl['bytes']:
                raise ValueError('slice size mismatch')
            uploads.append(file)
            lines.append(f'load {file.name} {stream_end:#x}')
            slices.append((pic, index, stream_end, sl['bytes']))
            stream_end += (sl['bytes'] + 4095) & ~4095
    if stream_end > 0x100000 or len(trace) != len(slices):
        raise ValueError('input region or trace/slice count mismatch')
    lines.append('sync')
    dumps = []
    guards = {}
    for run_index, (writes, (pic, sl, source, size)) in enumerate(zip(trace, slices)):
        ptr, table = None, {}
        slot = next(v for o, v in writes if o == 0x24c)
        for off, value in writes:
            if off == 0x2e0:
                ptr = value
            elif off == 0x2e4:
                table[ptr] = value
                ptr += 4
        address = table[0x400 + 32 * slot + 12]
        ptr = None
        lines.append(f'echo slice {run_index} picture {pic["picture"]}')
        for off, value in writes:
            if off == 0x2e0:
                ptr = value
                lines.append(f'wr {off:#x} {value:#x}')
            elif off == 0x2e4:
                relative = ptr - 0x400
                entry, word = divmod(relative // 4, 8)
                if 0 <= relative < 17 * 32 and word in (3, 4, 5, 6) and value:
                    base = 0x400 + entry * 32
                    luma, mv = table[base + 12], table[base + 20]
                    half = table[base + 24] - mv
                    if word in (3, 4):
                        value = allocate(buffers, luma, frame_size)
                        if word == 4:
                            value += stride * h
                    else:
                        # Match driver's conservative size and keep a canary beyond it.
                        need = ((w // 16) * (h // 16 + 1) * 32 * mv_factor + 1024 + 4095) & ~4095
                        fresh = mv not in motion
                        value = allocate(motion, mv, need + 4096)
                        if fresh:
                            guard = value + need
                            guards[mv] = guard
                            lines.append(f'fill {guard:#x} 0x1000 0xa5')
                            lines.append('sync')
                        if word == 6:
                            value += half
                    lines.append(f'wrpa {off:#x} {value:#x}')
                else:
                    if clear_modes and 0 <= relative < 17 * 32 and word == 2:
                        value = 0
                    # M027 laboratory probe: only the metadata word of a
                    # classified long-term entry changes; addresses stay intact.
                    if (long_term_mask and 0 <= relative < 17 * 32 and word == 2
                            and entry in longterm[pic['picture']]):
                        value |= long_term_mask
                    lines.append(f'wr {off:#x} {value:#x}')
                ptr += 4
            elif off in (0x250, 0x254):
                lines.append(f'wrpa {off:#x} {0x100000 if off == 0x250 else 0x180000:#x}')
            elif off == 0x230:
                lines.append(f'wrvld 0x230 {source:#x} 7')
            elif off == 0x23c:
                lines.append(f'wrpa 0x23c {source + size:#x}')
            elif off == 0x238:
                lines.append(f'wr 0x238 {size * 8}')
            elif off == 0xc4:
                lines.append(f'wr 0xc4 {stride * h // 4}')
            elif off == 0xc8:
                lines.append(f'wr 0xc8 {(stride // 2) << 16 | stride}')
            elif off == 0x228:
                # W1C: clear the previous slice's completion before polling the next.
                lines.append(f'wr 0x228 {value:#x}')
            else:
                lines.append(f'wr {off:#x} {0 if off == 0x220 else value:#x}')
                if off == 0x224 and value not in (7, 8):
                    lines.append('poll 0x228 0x100 0 200')
                elif off == 0x224 and value == 8:
                    lines.extend(['poll 0x228 0x1 0x1 1000', 'rd 0x228'])
        if sl == len(pic['slices']) - 1 and (not pic['field'] or pic['second']):
            name = f'frame-{pic["frame"]}.nv12'
            lines.extend(['sync', f'dump {buffers[address]:#x} {frame_size:#x} {name}'])
            dumps.append((pic, name))
    for index, guard in enumerate(guards.values()):
        lines.extend(['sync', f'dump {guard:#x} 0x1000 guard-{index}.bin'])
    script = directory / 'replay.lab'
    script.write_text('\n'.join(lines) + '\n')
    return uploads + [script], dumps, len(guards)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path)
    p.add_argument('manifest', type=Path)
    p.add_argument('stream', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--first-run', type=int, default=0)
    p.add_argument('--clear-modes', action='store_true')
    p.add_argument('--mv-factor', type=int, choices=(1, 2, 4), default=1)
    p.add_argument('--canon', type=Path, help='canon.c output for this exact clip')
    p.add_argument('--long-term-mask', type=lambda s: int(s, 0), default=0,
                   help='lab-only low-byte SRAM mode-word probe; M027 measured 0x11')
    args = p.parse_args()
    if not 0 <= args.long_term_mask <= 0xff:
        p.error('long-term mask must preserve the measured field/MBAFF mode bits')
    if args.long_term_mask and (not args.canon or args.clear_modes):
        p.error('long-term probe requires --canon and cannot combine with --clear-modes')
    args.output.mkdir(exist_ok=False)
    pictures = json.loads(args.manifest.read_text())
    count = sum(len(pic['slices']) for pic in pictures)
    trace = runs(args.trace)[args.first_run:args.first_run + count]
    uploads, dumps, guards = build(pictures, trace, args.manifest.with_suffix(''),
                                    args.output, args.clear_modes, args.mv_factor,
                                    long_term_slots(args.canon) if args.canon else None,
                                    args.long_term_mask)
    # These short clips have one POC epoch; later IDRs/MMCO5 need a separate mapper.
    if any(pic['mmco5'] or (i and pic['idr']) for i, (pic, _) in enumerate(dumps)):
        raise ValueError('multiple POC epochs')
    guest_dir = '/tmp/ve/' + args.output.name
    if not all(c.isalnum() or c in '-_' for c in args.output.name):
        raise ValueError('unsafe run name')
    with (ROOT / 'linux-work/h264-traces/.board-cycle.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        subprocess.run(SSH + [f'mkdir {guest_dir}'], check=True, timeout=20)
        subprocess.run(SCP + [str(f) for f in uploads] + [f'root@192.0.2.82:{guest_dir}/'], check=True, timeout=60)
        actual = subprocess.check_output(SSH + [f'cd {guest_dir} && md5 -q ' + ' '.join(f.name for f in uploads)], timeout=30).decode().splitlines()
        expected = [hashlib.md5(f.read_bytes()).hexdigest() for f in uploads]
        if actual != expected:
            raise ValueError('guest upload MD5 mismatch')
        result = subprocess.run(SSH + [f'cd {guest_dir} && ../ve_lab replay.lab'], capture_output=True, timeout=120)
        (args.output / 'lab.txt').write_bytes(result.stdout + result.stderr)
        statuses = [int(v, 16) for v in re.findall(rb'^228 = ([0-9a-fA-F]+)$',
                                                  result.stdout, re.M)]
        if (result.returncode or b'TIMEOUT' in result.stdout or len(statuses) != count or
            any(v & 7 != 1 for v in statuses)):
            raise RuntimeError('lab failed: inspect lab.txt')
        names = [name for _, name in dumps] + [f'guard-{i}.bin' for i in range(guards)]
        for name in names:
            subprocess.run(SCP + [f'root@192.0.2.82:{guest_dir}/{name}', str(args.output / name)], check=True, timeout=30)
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-xerror', '-threads', '2', '-apply_cropping', '0',
                                   '-i', str(args.stream), '-pix_fmt', 'nv12', '-fps_mode', 'passthrough',
                                   '-f', 'rawvideo', 'pipe:1'])
    w, h = pictures[0]['width'], pictures[0]['height']
    size = w * h * 3 // 2
    if len(raw) != size * len(dumps):
        raise ValueError('reference frame count mismatch')
    report = []
    for display, (pic, name) in enumerate(sorted(dumps, key=lambda item: item[0]['poc'])):
        actual = (args.output / name).read_bytes()
        expected = raw[size * display:size * (display + 1)]
        report.append({'frame': pic['frame'], 'poc': pic['poc'],
                       'matching_bytes': sum(a == b for a, b in zip(actual, expected)),
                       'bytes': size, 'exact': actual == expected})
    intact = all((args.output / f'guard-{i}.bin').read_bytes() == b'\xa5' * 4096 for i in range(guards))
    result = {'frames': report, 'mv_guards_intact': intact}
    (args.output / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return 0 if intact and all(r['exact'] for r in report) else 1


if __name__ == '__main__':
    raise SystemExit(main())
