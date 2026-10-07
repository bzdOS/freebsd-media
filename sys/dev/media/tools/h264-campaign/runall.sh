#!/bin/bash
# DPB comparison against ffmpeg over the whole corpus
cd /opt/bzdos/linux-work/jvt
python3 - <<'PY' > /tmp/dpb_list.txt
import json
c=json.load(open('catalog.json'))
for n,e in sorted(c.items()):
    if e.get('ffmpeg_ok') and not e.get('num_slice_groups_minus1') and e.get('chroma_format_idc') in (None,1,0) and e.get('profile') not in ('Extended',):
        print(n, e['bitstream'])
PY
while read n b; do
  r=$(timeout 120 python3 /opt/bzdos/linux-work/h264front/dpbcmp.py "$b" ${1:-50} 2>&1 | tail -3 | tr '\n' ' ')
  echo "$n: $r"
done < /tmp/dpb_list.txt
