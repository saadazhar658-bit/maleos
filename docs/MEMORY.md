# Memory Management

Phase 2 design notes. Source lives in `src/kernel/mm/`.

## Boot sequence

1. **GRUB** loads the kernel at 1 MiB (physical) in 32-bit protected mode and passes a Multiboot2 info pointer.
2. **`boot.asm`** builds temporary page tables (the first 1 GiB mapped both at `0x0` and at `0xFFFFFFFF80000000`), enables PAE, long mode, NX, paging and `CR0.WP`, then jumps into the higher half.
3. **`bootinfo_parse()`** copies the memory map out of boot memory into a static `struct boot_info`.
4. **`pmm_init()`** builds the frame bitmap from that map.
5. **`vmm_init()`** builds the real kernel address space and switches `CR3`. The identity map is gone from this point on.
6. **`heap_init()`** maps the first heap pages and `kmalloc`/`kfree` become available.
7. **`mm_selftest()`** verifies all of the above.

## Physical memory manager (`pmm.c`)

- One bit per 4 KiB frame (`1` = used). The bitmap is static (256 KiB), so it needs no allocator to bootstrap.
- Everything starts as used. Regions the firmware reports as usable are released, then anything non-usable that overlaps is re-reserved.
- Always reserved: the first 1 MiB, the kernel image, and the Multiboot2 info structure.
- `pmm_alloc_frame()` returns the lowest free frame (first fit from a rotating hint) and `0` on failure. Frame 0 is never free, so `0` is an unambiguous error value.
- `pmm_alloc_frames(n)` returns `n` physically contiguous frames (linear scan).
- Freeing an address that is unaligned, out of range, or already free is a kernel panic.
- **Limit:** RAM above `PMM_MAX_PHYS` (8 GiB) is ignored.

## Virtual memory manager (`vmm.c`)

Four-level x86_64 paging with 4 KiB leaf pages.

| API | Purpose |
|---|---|
| `vmm_map(pml4, virt, phys, prot)` | Map one page; allocates page tables on demand |
| `vmm_unmap(pml4, virt)` | Remove a mapping (does not free the frame) |
| `vmm_protect(pml4, virt, prot)` | Change permissions |
| `vmm_translate(pml4, virt, &phys, &prot)` | Walk the tables (understands 2 MiB and 1 GiB pages) |

`prot` is a combination of `VMM_WRITE`, `VMM_EXEC`, `VMM_USER`, `VMM_NOCACHE`. Read is always implied.

### Protection policy

- **W^X:** `VMM_WRITE | VMM_EXEC` is rejected with `VMM_EINVAL`.
- **NX:** every page that is not explicitly executable gets the NX bit.
- **`CR0.WP`:** set during boot so the kernel cannot write through read-only mappings.
- **Guard pages:** the lowest page of the kernel stack is left unmapped, and the heap is followed by unmapped memory. A stack overflow lands on the guard page and is reported as a double fault on a dedicated IST stack.

### Kernel image mapping

| Section | Permissions |
|---|---|
| `.text` | R + X |
| `.rodata` | R |
| `.data`, `.bss` | R + W, NX |
| stack guard page | unmapped |

## Kernel heap (`heap.c`)

- Region: `0xFFFFC00000000000`, up to 256 MiB, mapped RW+NX a few pages at a time.
- Blocks carry a 32-byte header (magic, size, previous size, free flag). Allocation is first fit with splitting. Freeing coalesces with both neighbours in O(1).
- Payloads are 16-byte aligned. Freed memory is poisoned with `0xDD`.
- A bad magic value, a double free, or a pointer outside the heap panics immediately.
- `heap_check()` walks every block and validates the invariants.
- API: `kmalloc`, `kzalloc`, `krealloc`, `kfree`, `heap_get_stats`.

## Known limitations (candidates for later phases)

- No locking yet. Add spinlocks in Phase 3 before the heap or PMM are used from more than one context.
- Allocation is a linear first-fit walk. Fine for now; size-class free lists can come later.
- Page tables are never freed when empty.
- Only 4 KiB mappings are exposed through `vmm_map`; the direct map uses 2 MiB pages internally.
- Device memory (APIC, PCI BARs) needs an `ioremap`-style helper with `VMM_NOCACHE` (Phase 3/4).

## Testing

`mm_selftest()` runs 117 checks on every boot (frame accounting, map/unmap/protect, W^X rules, kernel section permissions, guard page, direct map, removal of the identity map, heap alignment/reuse/coalescing/growth/realloc/stress). `make test` and CI require the `MM SELFTEST: PASS` line.

Faults were also verified by hand in QEMU: writing to `.text` or `.rodata`, executing from the heap, reading past the heap end, touching the removed identity map, and overflowing the kernel stack all produce the expected exception report.
