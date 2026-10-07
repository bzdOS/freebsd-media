# usage: sh assay.sh "label" "sysctl a=b c=d" runs  -- smb under 2 dd hogs; per-slice error rate
cd /tmp/ve
for kv in $2; do sysctl dev.cedrus.0.$kv >/dev/null; done
sl() { sysctl -n dev.cedrus.0.h264_last | sed 's/.*slices \([0-9]*\).*/\1/'; }
dd if=/dev/zero of=/dev/null bs=8m count=1000000 2>/dev/null & h1=$!
dd if=/dev/zero of=/dev/null bs=8m count=1000000 2>/dev/null & h2=$!
sleep 1
e0=$(sysctl -n dev.cedrus.0.errors); s0=$(sl)
i=0; while [ $i -lt $3 ]; do ./cedrus_h264 -n 3 smb.264 smb.yuv >/dev/null 2>&1; i=$((i+1)); done
e1=$(sysctl -n dev.cedrus.0.errors); s1=$(sl)
kill $h1 $h2 2>/dev/null; wait 2>/dev/null
echo "$1 [$2] slices=$((s1-s0)) errors=$((e1-e0))"
