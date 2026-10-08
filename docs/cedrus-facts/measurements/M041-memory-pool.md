# M041: a reserved contiguous buffer pool for the engine, 2026-10-08

Problem (F:M040-d): the engine needs physically contiguous buffers (1080p: ~3 MB OUTPUT, ~3 MB CAPTURE each, MV and
scratch buffers of a few MB). On a guest that has been up for a while with the GPU stack loaded `bus_dmamem_alloc` /
`contigmalloc(M_NOWAIT)` fail with ENOMEM (FFmpeg: `Failed to create buffer of type 1: Cannot allocate memory`).

## What was built

- `sys/dev/cedrus/cedrus_pool.c`: at attach, once, `contigmalloc` of `hw.cedrus.pool_mb` MiB (default 64, 0 = old
  behaviour) from the engine's reach (tried below the first 256 MiB above `cedrus_dram_base`, then wider), set
  uncacheable once for the whole pool (M009), carved page by page by a bitmap, first fit (a bitmap coalesces by
  construction; freeing allocates nothing and cannot fail). Allocations are zeroed. Sysctls `dev.cedrus.0.pool_kb`,
  `pool_used_kb`, `pool_largest_kb`, `pool_live`, `pool_fails`, `pool_allocs`. `cedrus_detach` returns EBUSY while
  `pool_live != 0`.
- Media core: a new optional method `vbq_alloc_mem(q, b, size)` (`vbq_if.m`). Default returns EOPNOTSUPP and the core uses
  bus_dma as before (vtest, vconv are unchanged). A driver that returns 0 has filled `b->vaddr/paddr/memattr/mem_free`;
  the core then treats the buffer as one physical segment for mmap faults and for the dma-buf scatter list (EXPBUF), and
  calls `b->mem_free` at the last reference (it may run after the queue is gone, so it does not use `b->q`).
- Cedrus returns ENOMEM from the method when the pool has no room, so the core falls back to bus_dma (counted in
  `pool_fails`). `cedrus_mem_alloc` (H.264 MV and scratch) tries the pool first, then the old ladder.

## Measurements (one board, A64, 1.8 GB guest, GPU stack loaded: lima, drm, dmabuf)

Fragmenter: `tests/frag.c MB` maps MB of anonymous memory, touches it, wires it (`mlock`; user pages that are not wired
are moved by contigmalloc's page reclaim, which is why a first try with unwired pages did not reproduce the failure),
then unmaps every other page: free memory is single pages. `tools/pool-test.sh frag MB` runs it and decodes 1080p
(`p1080.264`, 150 frames, `-hwaccel v4l2request -hwaccel_output_format drm_prime`) three times.

- F:M041-a  **Old code, not fragmented** (guest up 1:45, v_free_count 232630): 3/3 decodes of p1080 without errors.
- F:M041-b  **Old code, fragmented** (frag 840 MB, wired, 120472 free pages left, all single): 3/3 decodes fail with
  `Failed to create buffer of type 1: Cannot allocate memory (12)` and `hwaccel initialisation returned error`. The same
  run without wiring (user pages): 3/3 succeed. 
- F:M041-c  **Fresh boot with pool** (media + cedrus from this branch, md5 verified, `cedrus0: buffer pool: 64 MiB at pa
  0x51000000`): `all.sh 2 idle` 12 runs of 6 streams, 0 bad; FFmpeg `ffmpeg-test.sh f3 smb wb t640 hd640 hd1080`: 6/6
  byte-identical to the reference (0 differing samples; hd1080 18,800,640 samples). After it: pool_allocs 558,
  pool_fails 4, pool_live 0, used 0. The 4 fails are requests the 64 MiB pool could not place at that moment (the
  1080p stream wants about 50 MB: see (e)); they fell back to bus_dma and the runs passed.
- F:M041-d  **New code, fragmented, no reboot:** frag 840 MB (217756 free pages left) and frag 1200 MB (168327 free
  pages left, all single): 3/3 and 3/3 decodes of p1080 without an error line; pool_allocs grew by 72 per three runs
  (the hardware path was used), pool_fails stayed 4, used 0 afterwards.
  Not run: the same fragmented state with `hw.cedrus.pool_mb=0` on the new code (another agent's player held the
  device); the contrast is (b), on the old module.
- F:M041-e  A 1080p stream in flight held used=49,520 KiB, live=26 of the 65,536 KiB pool; `kldunload cedrus` then
  failed with `Device busy`, and after the stream ended used was 0.
- F:M041-f  **20 open/decode/close cycles** (ffmpeg, 10 frames of p1080 each): 20/20 ok, pool_used_kb 0 before and
  after, live 0, allocs 1220 -> 1708 (488), fails unchanged. A first run of the same loop while another agent's
  `fbplay` held the device read 4896 KiB / 22 live before and after (no change over 20 cycles); it is not the proof,
  the second run is.

## Traps

- The pool is taken at attach: if cedrus is loaded late in a fragmented guest the 64 MiB may not be there
  (`no N MiB for the buffer pool` is printed and the old path is used). Put it in kld_list.
- 64 MiB is enough for one 1080p stream at 3-4 pictures; more streams or more buffers spill to bus_dma (pool_fails).
  Raise `hw.cedrus.pool_mb` (kenv before kldload).
- Unwired user memory does not fragment against contigmalloc; wired/kernel memory does. Reproduce with wired pages.
- `hw.cedrus.pool_mb` is read at attach only. `sys/modules/cedrus/Makefile` now lists `clknode_if.h` so a clean object
  directory builds.
