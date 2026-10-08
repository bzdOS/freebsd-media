# M031 — SP requests reach A64 hardware, then fail

2026-10-08. Hardware-only scope; no software fallback. Production module is
still fbsd-media3cd9cd7, MD5 d57627e64bb4cfc21e64d264e9a19b5a.

## Front-end gap closed for research

Previously `h264ctl.h` rejected SP before any Linux request could be submitted.
fbsd-media **f4450d4** parses `sp_for_switch_flag` and `slice_qs_delta` in
non-strict research mode. Strict mode keeps its previous refusal; SI remains
refused. This is trace infrastructure, not production SP support.

Both official streams parse all 400 pictures. Their first SP picture is index10.
For the first SP slice, SP1_BT_A has header_bits75, qp_delta2, qs_delta1;
sp2_bt_b has header_bits76, qp_delta4, qs_delta3. Switch flags are zero.
Independent FFmpeg `trace_headers` agrees on the switching fields. All **223**
streams fully parsed by the old non-strict front end retain byte-identical
canonical control dumps. This count includes formats the native driver refuses;
it is not a hardware-support count. Strict old/new dumps for both SP clips match,
including `end 9 -2` (the parser looks ahead while delimiting the previous picture).
[Controls and regression](M031-sp/).

The captured header initially also passed SI syntax in research mode. The final
commit deliberately retains the old SI refusal because no SI fixture was tested.
The exact captured header and rig source are retained alongside the final SP
control dumps; the SP path is unchanged.

## Black-box Linux measurement

Two 12-picture clips were embedded, with H264_MAX_PICS=12. One locked Linux6.12
Image build completed with exit0. The versioned run-cycle captured **3918 MMIO
entries**, with zero invalid entries, then restored the FreeBSD TFTP pairing and
confirmed FreeBSD SSH plus UFS `/var`. No persistent media was flashed; no Linux
implementation source was read.

| Input | Completed prefix | First SP request |
|---|---|---|
| SP1_BT_A | 10/10 luma hashes match official YUV | ERROR buffer, status0x50010003 |
| sp2_bt_b | 10/10 luma hashes match official YUV | ERROR buffer, status0x40010003 |

There are22 slice runs: ten successful pictures plus one failed SP for each
stream. Both failed runs write SLICE register0x208 = **0x1320**, including
slice_type3. Thus these failures are beyond front-end rejection: SP reaches the
hardware. The trace observes this Linux programming path failing; it does not
prove that every possible SP programming sequence is unsupported by silicon.
Only prefix luma hashes were measured here, not complete Y/C equivalence.
[Trace](M031-sp/linux-trace.txt), [console](M031-sp/linux-console.txt),
[comparison](M031-sp/linux-comparison.json), [cycle](M031-sp/linux-result.json).

## SP reference discrepancy

Both full400-picture streams decode to15206400 bytes in JM19 and FFmpeg without
a process error, but neither decoder matches the supplied official YUV after
picture9. JM and FFmpeg also disagree with each other from the first SP picture.
All three outputs match exactly for the first10 pictures. Consequently neither
software result can silently replace the official SP oracle. This ambiguity is
independent of the observed hardware ERROR buffers. Full output files and JM
logs remain in `linux-work/h264-traces/m031-sp`; hashes are in
[jm-results.json](M031-sp/jm-results.json).

## Restored native state

After recovery, board-bootstrap restored the native modules and tools; uploaded
test sources and f3 inputs were MD5-verified. Native f3 passes **8/8 full Y/C**.
Fresh-boot counters: pictures8, errors0, timeouts0, rejects0; vbq live0,
cedrus_dec one1KiB allocation. The production module hash is unchanged.
[Recovery](M031-sp/recovery-f3.txt), [state](M031-sp/recovery-state.txt).

The measured hardware-support result remains M028's176/176 accepted streams,
19028/19028 exact frames, with M030's monochrome I_PCM caveat. The remaining55
corpus formats are still refused. M031 closes the missing SP trace, not SP
support. Further SP implementation needs a working hardware programming
contract and an independently resolved pixel oracle. The hardware-only231/231
objective remains unmet; M030 records the separate reported ten-bit hardware
limit. Task fedora-183 remains open.
