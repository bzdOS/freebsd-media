#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# Boot the Linux guest (HV Linux flavour, VETRAP=2) with the H.264 initramfs, dump console and VE trace
S=/tmp/h264-scratch
OUT=${1:-h264}
T=/opt/bzdos/tftpboot; B=$T/.fbsd-backup
restore() { cd $T && \cp -p $B/kernel kernel && \cp -p $B/bananapi-min.dtb bananapi-min.dtb && \cp -p $B/microkernel-dbg.uimg microkernel-dbg.uimg; echo restored >> $S/cycle.log; }
trap restore EXIT
: > $S/cycle.log
cd $T && \cp $B/hv-linux.uimg microkernel-dbg.uimg && \cp /opt/bzdos/linux-work/build-min/arch/arm64/boot/Image kernel && dtc -I dts -O dtb -o bananapi-min.dtb /opt/bzdos/microkernel/linux-guest/bpi_m64_trace.dts
cd /opt/bzdos/microkernel
python3 safemode.py force 0 >/dev/null 2>&1
ssh -i ~/.ssh/chimp_ed25519 -o ConnectTimeout=5 root@192.0.2.82 'shutdown -r now' </dev/null >/dev/null 2>&1
echo "reboot requested" >> $S/cycle.log
for i in $(seq 1 40); do sleep 5; timeout 60 python3 /opt/bzdos/microkernel/vcon_dump.py 6000 > $S/$OUT.con.txt 2>&1; if grep -q "TRACE-DONE" $S/$OUT.con.txt; then break; fi; done
echo "console polled" >> $S/cycle.log
timeout 120 python3 /opt/bzdos/microkernel/vcon_dump.py 30000 > $S/$OUT.con.txt 2>&1
timeout 280 python3 /opt/bzdos/microkernel/vetrace.py > $S/$OUT.trace.txt 2>$S/$OUT.trace.err
echo "trace dumped" >> $S/cycle.log
restore
python3 safemode.py force 0 >/dev/null 2>&1
python3 -c "
from hvdbg import HV
HV().wdt_reset()" >/dev/null 2>&1
echo "board reset to FreeBSD" >> $S/cycle.log
