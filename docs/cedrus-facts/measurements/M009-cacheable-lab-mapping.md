# M009: the lab failures were a cacheable user mapping (2026-10-05)

Resolves the open question of M007-f and M008: why textured streams fail in the
FreeBSD lab while Linux decodes them.

- F:M009-a  The decode failures do not depend on polling. With no register read
  between the trigger and a 300 ms wait, i64 still ends with 0x1c4 = 9.
- F:M009-b  They depend on where the stream lies in the buffer. i64 at offsets 0,
  0x40, 0x80 and 0x200 fails differently. The all-gray flat stream, clean at
  offset 0, fails completely at 0x80, 0x100 and 0x3d0 (0x1c4 = 1, no output).
  Same bytes, different place: the engine was not reading what the CPU wrote.
- F:M009-c  Cause: the user mapping of the lab buffer was write-back. The driver's
  d_mmap returned VM_MEMATTR_UNCACHEABLE, but the device pager takes the attribute
  of a real page from the page itself (pmap_page_get_memattr) and only prints
  "memattr inconsistently (drv 1 pmap 2)" (1 = uncacheable, 2 = write-back on
  arm64). pmap_change_attr on the kernel address changes the kernel mappings, not
  the pages' attribute. So stream bytes sat in the CPU cache; the engine, which is
  not coherent, read stale DRAM. Hypervisor reads of the buffer went through a
  cacheable mapping and saw the cache, which is why the data looked right in DRAM
  (M008-f's "verified from the hypervisor" did not verify DRAM).
- F:M009-d  Check: cleaning and invalidating the buffer from userland (dc civac)
  before the trigger and before reading the output gives 0x1c4 = 0 and the full
  picture at every offset tried, for i64, flat and grad (15 of 15 runs).
- F:M009-e  Fix in the driver: pmap_page_set_memattr(UNCACHEABLE) on every page of
  the buffer at attach (WRITE_BACK again before contigfree). With it, and no cache
  operations in the script, i64 with Linux's exact register sequence (0x114 =
  0x800000b8, VBV_END = source + length, polling) decodes 20 of 20 times with
  identical output: status 0xc001, 0x1c4 = 0, equal to ffmpeg in Y 4061/4096 and
  C 2023/2048, maximum difference 1. These are Linux's figures (M008-d).
- F:M009-f  Consequences for earlier facts. M007-a's need for VBV_END past the data,
  M007-c's missing last row, M007-d's "0xb8 gives no output", M007-f, M008-c's
  flakiness and M008-f's reset dependence were all observed through the stale
  cache and are void: with an uncached buffer the Linux sequence works as is.
- Rule for the real driver: every buffer the engine reads or writes must either
  have its pages marked uncached (the attribute lives in vm_page, not in d_mmap)
  or get explicit cache maintenance around each job. The M3 buffer queue already
  takes the attribute from the page (sys/dev/media/vbq.c).
