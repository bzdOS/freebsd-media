#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""corpus-sweep.py [first_n] [name ...]
Decode the first packets of every supported stream of the JVT conformance corpus (linux-work/jvt, see
tools/h264-campaign/catalog.py) with FFmpeg's V4L2 Request hwaccel on the board, and compare the md5 of the raw
output with the software decode of the same cut stream made on the host. Results: corpus-sweep.txt."""
import json, subprocess, sys, os, hashlib
K = '/root/.ssh/chimp_ed25519'; G = 'root@192.0.2.82'
ROOT = '/opt/bzdos/linux-work/jvt'
OUT = '/opt/bzdos/linux-work/ffmpeg/corpus'
os.makedirs(OUT, exist_ok=True)
n = int(sys.argv[1]) if len(sys.argv) > 1 else 40
only = sys.argv[2:]
cat = json.load(open(ROOT + '/catalog.json'))
def supported(e):
    return (e.get('ffmpeg_ok') and not e.get('num_slice_groups_minus1') and e.get('chroma_format_idc') in (None, 1, 0)
            and e.get('bit_depth_luma_minus8') in (None, 0) and e.get('profile') not in ('Extended',)
            and 'bitstream' in e)
def ssh(cmd, timeout=300):
    return subprocess.run(['ssh', '-i', K, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', G, cmd], capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
res = open(OUT + '/corpus-sweep.txt', 'a')
done = set(l.split()[0] for l in open(OUT + '/corpus-sweep.txt')) if os.path.getsize(OUT + '/corpus-sweep.txt') else set()
ok = bad = 0
for name, e in sorted(cat.items()):
    if not supported(e) or (only and name not in only) or name in done:
        continue
    cut = '%s/%s.264' % (OUT, name)
    r = subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', e['bitstream'], '-c', 'copy', '-frames:v', str(n), '-f', 'h264', cut])
    sw = subprocess.run(['ffmpeg', '-v', 'error', '-i', cut, '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-'], capture_output=True)
    want = hashlib.md5(sw.stdout).hexdigest(); size = len(sw.stdout)
    subprocess.run(['scp', '-q', '-i', K, cut, G + ':/var/tmp/cs.264'])
    t = ssh('export LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib; e0=$(sysctl -n dev.cedrus.0.errors); cd /var/tmp; '
            '/var/tmp/ffv4l2/bin/ffmpeg -v error -hwaccel v4l2request -i cs.264 -an -pix_fmt yuv420p -f rawvideo - 2>/var/tmp/cs.err | md5 -q; '
            'echo errors $(( $(sysctl -n dev.cedrus.0.errors) - e0 )); head -c 300 /var/tmp/cs.err | tr "\\n" " "')
    lines = t.stdout.strip().split('\n') if t.stdout else ['']
    got = lines[0].strip()
    extra = ' '.join(lines[1:])
    status = 'EXACT' if got == want else 'DIFFERENT'
    ok += status == 'EXACT'; bad += status != 'EXACT'
    res.write('%s %s %s frames_bytes=%d %s\n' % (name, status, e.get('profile'), size, extra)); res.flush()
    print(name, status, extra, flush=True)
print('exact', ok, 'different', bad)
