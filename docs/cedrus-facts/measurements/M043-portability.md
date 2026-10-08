# M043 — portability: the SoC table, the DRAM base and the DMA span

2026-10-08. Block D. Code: `sys/dev/cedrus/cedrus_soc.h` (the table's structure and what differs on other
Allwinner engines), `cedrus_addr.h` (the address arithmetic, shared with a host test),
`cedrus.c` (`cedrus_socs[]`, `cedrus_dma_setup`, `cedrus_find_blocks`).

## F:M043-a — the "256 MiB window" is not a limit; the field is 32 bits

The VLD address word carries address bits 27:4 in 27:4 and bits 31:28 in 3:0 (F:M003-a); plane and table
addresses are plain 32-bit values. The engine's address is therefore `pa - dram_base` as a **32-bit** number, a
4 GiB reach, of which the A64 (at most 2 GiB DRAM) can use 2 GiB. M003-a only said the 256 MiB window was
"consistent with" the layout, "not yet proven"; M018-c and M027-a already decoded with buffers up to 1 GiB
above the base. This session adds (board, below) exact decodes with source buffers at relative address
0x59b20000 (1.4 GiB, address nibble 5) and the whole run with `dma_mb=src_mb=1792` (all RAM of the guest:
0x40000000..0xadaec000) with zero errors. So:

- **What limits buffer placement on a bigger DRAM:** nothing below 2 GiB above `dram_base`; memory above
  `dram_base + window` (a board with more than 2 GiB, or any RAM above the window) must not be handed to the engine.
  The driver enforces this three ways: `cap.dma_maxaddr`/`out.dma_maxaddr` = `dram_base + min(dma_mb|src_mb, window) - 1`
  (vbq turns it into the `lowaddr` of the `bus_dma_tag`, so `bus_dmamem_alloc` stays under it); `cedrus_mem_alloc`
  (motion-vector and scratch buffers) calls `contigmalloc` with `high = dram_base + hi - 1` where the ladder never
  exceeds `min(max(dma_mb, 1 GiB), window)`; and every picture is checked with `cedrus_in_dma()` before the engine
  is programmed (destination, source, references), an out-of-span buffer is rejected (EINVAL) or, for a missing
  reference, replaced by the picture's own buffer (counter `missing_refs`).
- **What vbq cannot do** (core, documented, not changed): it has only an upper bound (`dma_maxaddr`). A lower bound is
  not needed on the A64 because nothing lies below the engine's address 0; a SoC whose engine address 0 is above some RAM
  would need a `dma_minaddr` in `struct vbq`, or the `highaddr` hole of the bus_dma tag.
- The default placement span stays the measured 1 GiB (`dma_default`, M027-e); the guest has 1.73 GiB of RAM, so
  0.73 GiB is never used for buffers by default, and `dma_mb` up to the window can be set (takes effect at open).

## F:M043-b — what is derived and from where

| item | source |
|---|---|
| SoC (flags, clock names, fall-backs) | `compatible` -> `cedrus_socs[]` |
| engine address 0 (`dram_base`) | table (A64: 0x40000000, a property of the SoC memory map); loader tunable `hw.cedrus.dram_base` overrides for experiments (prints a NOTE) |
| reach (`dma_window`) | table (A64: 2 GiB); `hw.cedrus.dma_window_mb` can only shrink it |
| RAM inside the reach | `vm_phys_segs` (what the kernel took from the FDT memory node or EFI map): `dev.cedrus.0.mem_lo`/`mem_hi`, a message about RAM above the reach and below the base; **no RAM in the reach: attach fails with a message (ENXIO)** before any engine access |
| CCU and system-control addresses | device tree: provider of the first `clocks` entry; two levels above the `allwinner,sram` section; table values only as fall-back |
| pll_ve gating workaround | flag `CEDRUS_F_PLL_VE_UNGATED`; `hw.cedrus.pll_gate=0/1` overrides; a no-op when the PLL is already enabled |
| SRAM C1 claim | `sram_claim` sysctl (research), register offset from the table |

Read-outs: `dev.cedrus.0.dram_base`, `dma_window`, `mem_lo`, `mem_hi`, `dma_mb`, `src_mb`. New experiment knob:
`aux_lo_mb` (engine's own buffers are placed above this many MiB; emulates RAM that starts higher). No existing knob was
removed: every one of them (`mv_kb`, `scratch_kb`, `aux_near`, `aux_mb`, `src_mb`, `dma_mb`, `mod_mhz`, `h264_oldskip`,
`h264_hdrmode`, `poke*`, `peek`, `sram0`, `sram_claim`, `dump`) is documented in HANDOFF/M0xx or used by the lab.

## F:M043-c — what still assumes this board (honestly)

- `dram_base` is a SoC constant here; a guest whose memory does not start at 0x40000000 works only while IPA == PA (M004).
  Bare-metal A64 boards: RAM always starts at 0x40000000, so that case needs nothing, but **it was not run on bare metal**:
  only the chimp guest (RAM 0x40000000..0xadaec000) and emulations below.
- The register programming (MPEG-2 at 0x100, H.264 at 0x200, SRAM layout, quantiser tables) is the A64/H3-family map
  and is not table-driven; another VE generation needs code, not only a row (see `cedrus_soc.h`).
- `vm_phys_segs` is read by structure layout (`start`, `end`): the module must be built against the kernel's headers
  (already the rule: `freebsd-src-earlyboot-wt`).
- The lab (`hw.cedrus.lab=1`, `cedrus_lab.h`) keeps its own 256 MiB placement span and the A64 register research; it is opt-in.
- The test rig (`board-bootstrap.sh`, `compliance.sh`...) names the chimp guest's ssh key and address; the driver does not.
- `pll_ve` gating and the VE enable word in the system control are A64 facts (flags), unverified for H3/H5/H6.

## F:M043-d — measurements

Host: `sys/dev/media/tools/cedrus-addr-test.c` compiles the driver's own `cedrus_addr.h` and checks the VLD word
layout (round trip over the whole 2 GiB reach, F:M003-a examples), window edges for 512 MiB..2 GiB DRAM and 256 MiB..2 GiB
limits, overflow and below-base rejection, a machine whose RAM starts at base + 256 MiB: all passed.

Board (guest, Banana Pi M64 under chimp; module md5 `5ecf31f99dc961ca8830baacf7dccfd6`, built with
`OBJ=/opt/bzdos/fbsd-obj/portab/cedrus xbuild.sh cedrus`):

- attach: `Allwinner A64: DRAM base 0x40000000, engine reach 2048 MiB (to 0xc0000000)`,
  `RAM inside the reach: [0x40000000, 0xadaec000)`; defaults `dma_mb`=`src_mb`=1024 as before.
- Regression, default settings: `all.sh 2 idle`: f3, smb, wb, t640, hd640, hd1080 **12/12 runs exact**, errors 0,
  timeouts 0, rejects 0 (156 pictures). FFmpeg v4l2-request `ffmpeg-test.sh f3 smb wb t640 hd640`: **all five exact**
  (0 differing samples).
- E1 `dma_mb=src_mb=1792`, three passes of f3/smb/t640/hd640/hd1080: all exact, errors/rejects/timeouts 0; source
  buffers seen at relative 0x59b20000, 0x55ec0000, 0x551a0000, 0x50000000; pictures at 0x67b80000.
- E2 `aux_lo_mb=512`: MV and scratch buffers at relative 0x21000000..0x23100000 (all above 512 MiB), all five exact.
- E4 `hw.cedrus.dma_window_mb=256` (module reloaded; emulates the old 256 MiB model): `RAM inside the reach: [0x40000000,
  0x50000000); 1494 MiB above it are never used for buffers`; all five streams exact, every buffer at relative < 0x10000000
  (src 0x0f9c0000, luma 0x0f5e0000, scratch up to 0x0fa00000), errors 0, rejects 0.
- E3 `hw.cedrus.dram_base=0xc0000000` (above all RAM; safe, no engine access): `no RAM inside the engine's reach
  [0xc0000000, 0x140000000): the decoder is not set up`, `attach returned 6`, no /dev/video0. Tunables removed, module
  reloaded: decoder back, f3 8/8 exact. A shifted-but-overlapping base was deliberately NOT tried: the engine would
  write pictures to the wrong physical addresses on the shared board.
