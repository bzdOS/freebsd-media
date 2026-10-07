#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Sequential corpus assay; every upload checked, every emitted frame accounted for."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path('/opt/bzdos')
BASE = ROOT / 'linux-work/h264-traces'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--frames', type=int, default=8, help='cut size; zero means full stream')
    p.add_argument('--interlaced', action='store_true')
    p.add_argument('--names', nargs='*')
    a = p.parse_args()
    a.output.mkdir(exist_ok=False)
    catalog = json.loads((ROOT / 'linux-work/jvt/catalog.json').read_text())
    chosen = [(n, v) for n, v in catalog.items() if
              (not a.names or n in a.names) and v.get('pix_fmt') == 'yuv420p' and
              not v.get('num_slice_groups_minus1') and
              (not a.interlaced or v.get('frame_mbs_only_flag') == 0)]
    ssh_opts = ['-i', '/root/.ssh/chimp_ed25519', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8',
                '-o', 'ControlMaster=auto', '-o', 'ControlPersist=60',
                '-o', f'ControlPath={BASE}/corpus-ssh-%C']
    ssh = ['ssh'] + ssh_opts + ['root@192.0.2.82']
    result = []
    with (BASE / '.board-cycle.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        identity = subprocess.check_output(ssh + [
            'uname -a; kldstat; md5 /tmp/cedrus.ko /tmp/ve/cedrus_h264 '
            '/tmp/ve/cedrus_h264.c /tmp/ve/h264ctl.h'], timeout=20)
        (a.output / 'guest-before.txt').write_bytes(identity)
        for name, entry in chosen:
            row = {'name': name}
            try:
                clip, ref = a.output / 'input.264', a.output / 'reference.yuv'
                if a.frames:
                    command = ['ffmpeg', '-v', 'error', '-y', '-i', entry['bitstream'], '-c', 'copy',
                               '-frames:v', str(a.frames), '-f', 'h264', str(clip)]
                    subprocess.run(command, check=True, capture_output=True, timeout=60)
                else:
                    shutil.copyfile(entry['bitstream'], clip)
                # Stream metadata may retain cropped dimensions despite apply_cropping=0.
                # Decoded frame dimensions describe the actual raw reference layout.
                probe_run = subprocess.run(['ffprobe', '-v', 'error', '-threads', '2', '-apply_cropping', '0',
                    '-select_streams', 'v:0', '-read_intervals', '%+#16', '-show_frames',
                    '-show_entries', 'frame=width,height', '-of', 'json', str(clip)],
                    capture_output=True, check=True, timeout=30)
                (a.output / (name + '.probe.txt')).write_bytes(probe_run.stderr)
                frames = json.loads(probe_run.stdout).get('frames', [])
                if not frames:
                    raise RuntimeError('ffprobe produced no decoded frame (see probe log)')
                probe = frames[0]
                r = subprocess.run(['ffmpeg', '-v', 'error', '-y', '-threads', '2', '-apply_cropping', '0',
                    '-i', str(clip), '-pix_fmt', 'yuv420p', '-fps_mode', 'passthrough',
                    '-f', 'rawvideo', str(ref)], capture_output=True, timeout=120)
                (a.output / (name + '.reference.txt')).write_bytes(r.stderr)
                if r.returncode:
                    raise RuntimeError('ffmpeg reference failed')
                size = probe['width'] * probe['height'] * 3 // 2
                count, rem = divmod(ref.stat().st_size, size)
                if not count or rem:
                    raise RuntimeError('reference count/size invalid')
                row.update(frames=count, reference_warnings=bool(r.stderr))
                if ref.stat().st_size + clip.stat().st_size > 220 * 1024 * 1024:
                    raise RuntimeError('full reference exceeds guest memory budget; needs streaming test')
                subprocess.run(['scp'] + ssh_opts + [str(clip), str(ref), 'root@192.0.2.82:/tmp/ve/'],
                               check=True, capture_output=True, timeout=90)
                hashes = [hashlib.md5(f.read_bytes()).hexdigest() for f in (clip, ref)]
                remote = subprocess.check_output(ssh + ['md5 -q /tmp/ve/input.264 /tmp/ve/reference.yuv'], timeout=20).decode().splitlines()
                if remote != hashes:
                    raise RuntimeError('upload checksum mismatch')
                row['md5'] = hashes
                row['provided_reference_match'] = any(
                    v.get('md5') == hashes[1] for v in entry.get('provided', [])) if not a.frames else None
                r = subprocess.run(ssh + ['cd /tmp/ve && ./cedrus_h264 input.264 reference.yuv'],
                                   capture_output=True, timeout=max(60, count * 2))
                output = r.stdout + r.stderr
                (a.output / (name + '.txt')).write_bytes(output)
                passed = len(re.findall(rb'^picture .*: PASS ', output, re.M))
                row.update(exit=r.returncode, passed=passed,
                           exact=r.returncode == 0 and passed == count and
                           not re.search(rb'FAIL|ioctl|did not complete|parse failed|no reference', output))
            except (subprocess.SubprocessError, RuntimeError) as exc:
                row.update(exact=False, error=str(exc))
            result.append(row)
            (a.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            print(f'{len(result)}/{len(chosen)} {name}: ' + json.dumps(row), flush=True)
    return 0 if all(r['exact'] for r in result) else 1


if __name__ == '__main__':
    raise SystemExit(main())
