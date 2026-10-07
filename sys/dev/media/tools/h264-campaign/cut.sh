#!/bin/bash
# cut.sh <catalog name> <frames>  -> cut/<name>.<frames>.264 (Annex B, whole access units)
n=$1; f=$2
b=$(python3 -c "import json;print(json.load(open('/opt/bzdos/linux-work/jvt/catalog.json'))['$n']['bitstream'])")
ffmpeg -y -v error -i "$b" -c copy -frames:v $f -f h264 /opt/bzdos/linux-work/h264front/cut/$n.$f.264 && ls -la /opt/bzdos/linux-work/h264front/cut/$n.$f.264
