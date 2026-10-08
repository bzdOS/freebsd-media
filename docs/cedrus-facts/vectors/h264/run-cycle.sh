#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# run-cycle.sh — boot the Linux H.264 register-trace rig on the board, capture the
# vetrap ring, and put the FreeBSD guest back. Rebuilt from an earlier session's
# linuxh264.sh with the dead scratchpad path replaced by this directory.
#
#   run-cycle.sh <tag>
#
# Assets come from tftpboot/.fbsd-backup (the FreeBSD pairing) and the HV linux flavour
# hv-linux.uimg.  The Linux Image is whatever build-min currently holds.
set -u
S=/opt/bzdos/linux-work/h264-traces
T=/opt/bzdos/tftpboot
B=$T/.fbsd-backup
M=/opt/bzdos/microkernel
OUT=${1:-h264}
LOG=$S/$OUT.cycle.log
K=~/.ssh/chimp_ed25519
G=root@192.0.2.82

log() { echo "$(date -Is) $*" >> "$LOG"; }
die() { log "FATAL: $*"; echo "FATAL: $*" >&2; exit 1; }

restore() {
	cd "$T" || return
	\cp -pf $B/kernel kernel
	\cp -pf $B/bananapi-min.dtb bananapi-min.dtb
	\cp -pf $B/microkernel-dbg.uimg microkernel-dbg.uimg
	log "restored FreeBSD pairing (kernel, dtb, microkernel-dbg.uimg)"
}
trap restore EXIT

: > "$LOG"
log "start tag=$OUT"

# --- preflight -------------------------------------------------------------
pgrep -f 'chimpd\.py' >/dev/null && die "chimpd.py is running (SESSION-RULES R3: it must be stopped by PID first)"
timeout 5 ping -c 1 -W 2 192.0.2.82 >/dev/null 2>&1 || die "guest does not answer ping"
GUEST_BEFORE=$(timeout 20 ssh -i $K -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=6 $G 'uname -r; uptime' 2>&1 | tr '\n' ' ')
log "guest before: $GUEST_BEFORE"
echo "$GUEST_BEFORE" > "$S/$OUT.guest-before.txt"

cd "$M" || die "no $M"
python3 safemode.py force 0 >"$S/$OUT.safemode-before.txt" 2>&1
log "safemode forced to 0"

# --- stage the Linux rig over TFTP ----------------------------------------
cd "$T" || die "no $T"
\cp -pf $B/hv-linux.uimg microkernel-dbg.uimg
\cp -pf /opt/bzdos/linux-work/build-min/arch/arm64/boot/Image kernel
dtc -I dts -O dtb -o bananapi-min.dtb $M/linux-guest/bpi_m64_trace.dts 2>>"$LOG" || die "dtc failed"
md5sum microkernel-dbg.uimg kernel bananapi-min.dtb >> "$LOG"
log "staged: hv-linux.uimg + $(stat -c%s kernel) B Image + bpi_m64_trace.dtb"

# --- reload into the Linux guest ------------------------------------------
timeout 20 ssh -i $K -o BatchMode=yes -o StrictHostKeyChecking=no $G 'shutdown -r now' >/dev/null 2>&1
log "reboot requested"

ok=0
for i in $(seq 1 40); do
	sleep 5
	timeout 60 python3 $M/vcon_dump.py 8000 > "$S/$OUT.con.txt" 2>&1
	if grep -q "TRACE-DONE" "$S/$OUT.con.txt"; then ok=1; log "TRACE-DONE seen after $((i*5))s"; break; fi
done
[ $ok = 1 ] || log "WARNING: TRACE-DONE not seen in 200s"
timeout 120 python3 $M/vcon_dump.py 40000 > "$S/$OUT.con.txt" 2>&1
log "console dumped ($(stat -c%s "$S/$OUT.con.txt") B)"

# --- the trace -------------------------------------------------------------
timeout 280 python3 $M/vetrace.py > "$S/$OUT.trace.txt" 2> "$S/$OUT.trace.err"
log "trace dumped ($(stat -c%s "$S/$OUT.trace.txt") B), stderr: $(head -c 200 "$S/$OUT.trace.err")"
grep -m1 'entries written' "$S/$OUT.trace.err" >> "$LOG"

# --- back to FreeBSD -------------------------------------------------------
restore
python3 $M/safemode.py force 0 >>"$S/$OUT.safemode-after.txt" 2>&1
timeout 120 python3 -c "
import sys; sys.path.insert(0, '$M')
from hvdbg import HV
r = HV().wdt_reset()
print('wdt_reset ->', r)
" >>"$LOG" 2>&1
sleep 45
GUEST_AFTER=$(timeout 25 ssh -i $K -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 $G 'uname -r; uptime' 2>&1 | tr '\n' ' ')
echo "$GUEST_AFTER" > "$S/$OUT.guest-after.txt"
log "guest after: $GUEST_AFTER"
log "done tag=$OUT"
echo "done: $OUT"
