#ifndef NOAH_PAGE_H
#define NOAH_PAGE_H

/*
 * Guest page geometry, shared by both architectures.
 *
 * Extracted from include/x86/vm.h, where it lived because that is where it was
 * written, not because it is x86-specific. A 4KiB-granule aarch64 translation
 * regime has exactly the same shape as x86-64 4-level paging: 4KiB leaves, nine
 * address bits per level, 512 descriptors per table, and level sizes at 4KiB /
 * 2MiB / 1GiB. Only the descriptor *encoding* differs, and that stays in the
 * per-architecture headers.
 *
 * That coincidence is why the page arithmetic in src/mm/mmap.c and
 * src/proc/exec.c needs no porting at all - it was already
 * architecture-neutral, just not reachable without pulling in x86/vm.h.
 *
 * Note this is the *guest* granule. Stage 2 on Apple Silicon is fixed at 16KiB
 * by hv_vm_map and is a separate concern; see STAGE2_GRANULE in arm64/vm.h and
 * PORTING-arm64.md section 3.5.
 */

#include <stdint.h>

typedef enum {
  PAGE_4KB,
  PAGE_2MB,
  PAGE_1GB,
  PAGE_PML4E,   /* x86 name; on aarch64 this level is simply unused with a
                   39-bit VA, which starts the walk at the 1GiB level. */
} page_type_t;

#define PAGE_SHIFTOF(page_type)          (12 + (page_type) * 9)
#define PAGE_SIZEOF(page_type)           (1ULL << PAGE_SHIFTOF(page_type))
#define NR_PAGE_ENTRY                    512

static inline int is_page_aligned(void *addr, page_type_t page)
{
  return ((uint64_t)addr & (PAGE_SIZEOF(page) - 1)) == 0;
}

#endif
