#!/usr/bin/env python3
"""Validate each generated stream with external JM and FFmpeg executables."""
from pathlib import Path
import hashlib
import json
import subprocess

B = Path(__file__).resolve().parent
JM = Path('/opt/bzdos/linux-work/h264-traces/m027-oracle/JM-master/bin')
results = []
for item in json.loads((B / 'fixtures.json').read_text()):
    d = B / item['name']
    commands = [
        [str(JM / 'ldecod.exe'), '-d', str(JM / 'decoder.cfg'), '-p',
         f'InputFile={d / "stream.264"}', '-p', f'OutputFile={d / "jm.yuv"}',
         '-p', f'RefFile={d / "absent.yuv"}'],
        ['ffmpeg', '-v', 'error', '-i', str(d / 'stream.264'), '-pix_fmt',
         'yuv420p', '-f', 'rawvideo', '-y', str(d / 'ffmpeg.yuv')]
    ]
    exits = []
    for name, cmd in zip(('jm', 'ffmpeg'), commands):
        p = subprocess.run(cmd, cwd=d, capture_output=True, timeout=60)
        (d / f'{name}.txt').write_bytes(p.stdout + p.stderr)
        exits.append(p.returncode)
    a = (d / 'jm.yuv').read_bytes() if (d / 'jm.yuv').exists() else b''
    z = (d / 'ffmpeg.yuv').read_bytes() if (d / 'ffmpeg.yuv').exists() else b''
    size = item['width'] * item['height'] * 3 // 2
    report = dict(name=item['name'], exits=exits, jm_bytes=len(a), ffmpeg_bytes=len(z),
                  two_frames=len(a) == len(z) == 2 * size,
                  decoders_agree=a == z and len(a) == 2 * size,
                  first_pcm_exact=a[:size] == b'\x80' * size,
                  jm_sha256=hashlib.sha256(a).hexdigest(),
                  ffmpeg_sha256=hashlib.sha256(z).hexdigest(),
                  second_values=sorted(set(a[size:])) if len(a) == 2 * size else [])
    (d / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    (d / 'decode-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    results.append(report)
    print(json.dumps(report), flush=True)
(B / 'validation.json').write_text(json.dumps(results, indent=2) + '\n')
