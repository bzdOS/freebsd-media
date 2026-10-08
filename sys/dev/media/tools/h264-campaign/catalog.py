#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Extract bitstream + reference yuv of each JVT conformance zip, decode with ffmpeg,
compare with the provided yuv, record the features (SPS/PPS/slice flags)."""
import sys, os, zipfile, subprocess, re, json, hashlib, glob
ROOT = '/opt/bzdos/linux-work/jvt'
OUT = ROOT + '/streams'
os.makedirs(OUT, exist_ok=True)
BS = ('.264', '.jsv', '.26l', '.jvt', '.avc', '.h264', '.bit', '.bits', '.jvc', '.mp4', '.mkv')
def pick(names):
    bs = [n for n in names if n.lower().endswith(BS) and '/' not in n.strip('/')]
    bs = bs or [n for n in names if n.lower().endswith(BS)]
    yuv = [n for n in names if n.lower().endswith('.yuv')]
    return bs, yuv
def feat(path):
    r = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'debug', '-i', path, '-c', 'copy', '-frames:v', '60', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                       capture_output=True, text=True)
    t = r.stderr + r.stdout
    f = {}
    def g(name, cast=int):
        m = re.search(r'\b' + name + r'\s+\S+\s+=\s+(-?\d+)', t)
        return cast(m.group(1)) if m else None
    for k in ('profile_idc', 'chroma_format_idc', 'bit_depth_luma_minus8', 'frame_mbs_only_flag', 'mb_adaptive_frame_field_flag',
              'pic_order_cnt_type', 'num_ref_frames', 'direct_8x8_inference_flag', 'entropy_coding_mode_flag', 'weighted_pred_flag',
              'weighted_bipred_idc', 'num_slice_groups_minus1', 'transform_8x8_mode_flag', 'redundant_pic_cnt_present_flag',
              'constrained_intra_pred_flag', 'qpprime_y_zero_transform_bypass_flag', 'separate_colour_plane_flag'):
        f[k] = g(k)
    f['field_pic_flag_any'] = bool(re.search(r'\bfield_pic_flag\s+\S+\s+=\s+1', t))
    f['slice_types'] = sorted(set(re.findall(r'\bslice_type\s+\S+\s+=\s+(\d+)', t)))
    f['nal_types'] = sorted(set(re.findall(r'\bnal_unit_type\s+\S+\s+=\s+(\d+)', t)))
    f['gaps'] = g('gaps_in_frame_num_value_allowed_flag')
    f['num_nals'] = len(re.findall(r'\bnal_unit_type', t))
    return f
cat = json.load(open(ROOT + '/catalog.json')) if os.path.exists(ROOT + '/catalog.json') else {}
for z in sorted(glob.glob(ROOT + '/*/*.zip')):
    name = os.path.basename(z)[:-4]
    if name in cat and not os.environ.get('REDO'):
        continue
    try:
        zf = zipfile.ZipFile(z)
    except Exception as e:
        cat[name] = {'error': 'zip: %s' % e}; continue
    bs, yuv = pick(zf.namelist())
    if not bs:
        cat[name] = {'error': 'no bitstream', 'files': zf.namelist()[:10]}; continue
    d = OUT + '/' + name
    os.makedirs(d, exist_ok=True)
    zf.extract(bs[0], d)
    for y in yuv: zf.extract(y, d)
    b = d + '/' + bs[0]
    ent = {'bitstream': b, 'zipdir': os.path.basename(os.path.dirname(z)), 'yuv': [d + '/' + y for y in yuv]}
    ent.update(feat(b))
    pf = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height,pix_fmt,profile,field_order,codec_name', '-of', 'json', b], capture_output=True, text=True)
    try:
        s = json.loads(pf.stdout)['streams'][0]; ent.update(s)
    except Exception:
        ent['probe'] = 'failed'
    cat[name] = ent
    print(name, ent.get('profile'), ent.get('width'), ent.get('height'), 'fmo' if ent.get('num_slice_groups_minus1') else '', 'field' if ent.get('field_pic_flag_any') else '', 'mbaff' if ent.get('mb_adaptive_frame_field_flag') else '', flush=True)
    json.dump(cat, open(ROOT + '/catalog.json', 'w'), indent=1)
json.dump(cat, open(ROOT + '/catalog.json', 'w'), indent=1)
