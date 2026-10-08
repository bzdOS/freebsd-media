#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Save named research artifacts; keep vendor objects and raw outputs outside git."""
from pathlib import Path
import hashlib
import json
import shutil

W = Path('/opt/bzdos')
FACTS = W / 'bsdOS/docs/cedrus-facts/measurements'
selected = []


def copy(source, destination):
    assert source.is_file(), source
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    selected.append(str(destination.relative_to(W / 'bsdOS')))


def group(source, destination, names):
    for name in names:
        copy(source / name, destination / name)


def inventory(source, destination):
    items = {}
    for path in sorted(source.rglob('*')):
        if path.is_file() and (path.suffix in ('.bin', '.yuv', '.nv12', '.so') or
                               path.name.endswith('.tar.gz')):
            with path.open('rb') as data:
                digest = hashlib.file_digest(data, 'sha256').hexdigest()
            items[str(path.relative_to(W))] = dict(bytes=path.stat().st_size,
                                                   sha256=digest)
    output = destination / 'raw-sha256.json'
    output.write_text(json.dumps(items, indent=2) + '\n')
    selected.append(str(output.relative_to(W / 'bsdOS')))


b = W / 'linux-work/h264-traces/m035-sp-minimal'
d = FACTS / 'M035-sp-minimal'
group(b, d, ['fixtures.py', 'validate.py', 'run-lab.py', 'fixtures.json',
             'stream-order.json', 'validation.json', 'linux-summary.json',
             'build-result.json', 'kernel-build.txt', 'rig-build.txt',
             'lab-results.json', 'lab-upload-md5.json', 'guest-run.sh',
             'bootstrap.txt'])
for name in json.loads((b / 'stream-order.json').read_text()):
    group(b / name, d / name, ['stream.264', 'slice_0_0.bin', 'slice_1_0.bin',
                             'params.json', 'slices.json', 'validation.json',
                             'decode-commands.json', 'jm.txt', 'ffmpeg.txt'])
    group(b / name / 'normal', d / name / 'normal',
          ['replay.lab', 'job.json', 'result.json', 'lab.txt', 'lab-exit.txt'])
linux = b.parent / 'm035-sp-linux'
group(linux, d / 'linux', ['trace.txt', 'trace.txt.err', 'console.txt',
                         'console-tail.txt', 'cycle.log', 'manifest.json',
                         'result.json', 'triage-before.txt', 'guest-after-7.txt'])
for n in range(1, 7):
    copy(linux / f'guest-after-{n}.txt.err', d / 'linux' / f'guest-after-{n}.txt.err')
inventory(b, d)

b = W / 'linux-work/cedarx-research'
d = FACTS / 'M036-cedarx'
group(b, d, ['cedar-probe.c', 'elf-loader.h', 'fetch-sdk.py', 'prepare.py',
             'run-probe.py', 'compare.py', 'sdk-provenance.json',
             'vendor-summary.json', 'prepare.json', 'prepare-build.txt',
             'h264-elf.txt', 'probe-source.md5', 'probe-build.txt',
             'probe-build-2.txt'])
paths = [x['path'] for x in json.loads((b / 'vendor-summary.json').read_text())]
assert len(paths) == len(set(paths)) == 27
for name in paths + ['control-1', 'control-2']:
    for file in ['probe.txt', 'mmio.txt', 'run.json', 'upload-md5.json',
                 'pixel-comparison.json']:
        if (b / name / file).is_file():
            copy(b / name / file, d / name / file)
for path in sorted(b.glob('*-runner.txt')):
    copy(path, d / path.name)
inventory(b, d)

b = W / 'linux-work/h264-traces/m037-fmo-one-mb'
d = FACTS / 'M037-fmo-one-mb'
group(b, d, ['fixtures.py', 'run.py', 'recovery.py', 'export-evidence.py',
             'validation.json', 'results.json', 'oracle-limit.json',
             'jm-build.txt', 'upload-md5.json', 'recovery.json',
             'final-state.txt', 'tftp-restored.json', 'f3-recovery.txt',
             'gray-weighted-recovery.txt', 'gray-mbaff-cabac1-recovery.txt'])
for name in ['raster', 'fmo']:
    group(b / name, d / name, ['stream.264', 'input.bin', 'params.json',
                             'probe.lab', 'jm.txt', 'decode-command.json',
                             'lab.txt', 'lab-exit.txt', 'result.json'])
inventory(b, d)

for name, working in [('M035-sp-minimal', 'linux-work/h264-traces/m035-sp-minimal'),
                      ('M036-cedarx', 'linux-work/cedarx-research'),
                      ('M037-fmo-one-mb', 'linux-work/h264-traces/m037-fmo-one-mb')]:
    path = FACTS / name / 'README.txt'
    path.write_text(
        'Exact executed A64 research sequences, reports and owned source snapshots.\n'
        f'Working directory: /opt/bzdos/{working}/. Restore tools there for reuse; '
        'saved JSON names the exact dependencies and commands.\n'
        'Generated compressed test inputs are included. Raw capture/reference/guard '
        'buffers remain in the working directory and are identified in raw-sha256.json.\n'
        'Vendor binaries and downloaded generic public API headers are not included. '
        'M036 fetch-sdk.py retrieves pinned external objects separately.\n'
        'These tools generate fixtures, observe hardware and compare output; they '
        'are not production software-decoder fallback components.\n')
    selected.append(str(path.relative_to(W / 'bsdOS')))

extra = ['docs/cedrus-facts/HANDOFF.md', 'docs/cedrus-facts/HANDOVER.md',
         'SESSION_RULES.md', 'INBOX.md',
         'docs/cedrus-facts/measurements/M035-minimal-switching.md',
         'docs/cedrus-facts/measurements/M036-vendor-cedarx.md',
         'docs/cedrus-facts/measurements/M037-fmo-single-macroblock.md']
paths = sorted(set(selected + extra))
(b / 'evidence-paths.json').write_text(json.dumps(paths, indent=2) + '\n')
print(len(paths), 'named files;', sum((W / 'bsdOS' / p).stat().st_size for p in selected),
      'evidence bytes')
