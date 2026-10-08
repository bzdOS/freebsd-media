# M004: is the engine blocked outside itself? (2026-10-05)

Hypotheses tested while the VE never touches DRAM.

## H-IPA: guest addresses are not physical -- REFUTED
chimp's stage 2 is an identity map (IPA=PA, `stage2_enable()` "identity IPA=PA";
vnet_emac.c treats guest PA as plain pointers). pmap_kextract() therefore gives
the real physical address the engine would see. No SMMU on the A64.

## H-MBUS: master gate in CCU / DRAM controller -- NOT SUPPORTED
Read from the guest (dev.cedrus.0.peek, no EL2 block on these windows):
- CCU 0x01c2015c (MBUS clock)  = 0x81000002  gated on, running
- CCU 0x01c200fc (MBUS reset)  = 0x80000000  deasserted
- CCU 0x01c202c4 (bus reset 1) = 0x00101c11  VE bit set (out of reset)
- DRAMCOM 0x01c62010.. : eight-byte pairs per master; bit0 set in every pair
  except 0x01c62018 (0x06000008). The GPU renders, so that bit is not a plain
  master enable; no write was made. Nothing here points at a VE gate.

## Result
The engine is clocked, out of reset, addresses are physical and MBUS runs; the
remaining suspects are inside the engine's own register file (init writes
outside 0x100-0x1ff, trigger/status protocol) or an undiscovered gate.
