cd /tmp/ve
R=$1; hog=$2
if [ "$hog" = hog ]; then dd if=/dev/zero of=/dev/null bs=8m count=1000000 2>/dev/null & h1=$!; dd if=/dev/zero of=/dev/null bs=8m count=1000000 2>/dev/null & h2=$!; sleep 1; fi
tot=0; badn=0
for f in *.264; do s=${f%.264}; [ -f $s.yuv ] || continue
  ok=0; bad=0; np=0
  for i in $(seq $R); do o=$(./cedrus_h264 $s.264 $s.yuv 2>&1); p=$(echo "$o" | grep -c ': PASS'); n=$(echo "$o" | grep -c '^picture'); if [ $p -ge 1 ] && [ $p -eq $n ] && ! echo "$o" | grep -qE "FAIL|did not complete|ioctl|Cannot|reject"; then ok=$((ok+1)); else bad=$((bad+1)); last="$o"; fi; np=$p; done
  tot=$((tot+ok+bad)); badn=$((badn+bad))
  echo "$s pics=$np ok=$ok bad=$bad"
  [ $bad -gt 0 ] && echo "$last" | tail -3
done
echo "TOTAL runs=$tot bad=$badn $hog"
[ "$hog" = hog ] && kill $h1 $h2
