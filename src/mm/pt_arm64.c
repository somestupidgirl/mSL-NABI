/*
 * aarch64 stage-1 translation tables.
 *
 * Replaces the x86 pml4/pdp pair in src/mm/mm.c and the 512 generated entries
 * in src/mm/pdp.h. See PORTING-arm64.md sections 3.5 and 3.5.1.
 *
 * The layout is a 39-bit VA space with a 4KiB granule, which walks three
 * levels:
 *
 *   level 1   bits 38:30   1GiB per entry   (table or block)
 *   level 2   bits 29:21   2MiB per entry   (table or block)
 *   level 3   bits 20:12   4KiB per entry   (page)
 *
 * TTBR0_EL1 points at the level-1 table. There is no level 0 - T0SZ=25 makes
 * the walk start at level 1 - and no TTBR1 at all, because the guest has no
 * high half (TCR_EL1.EPD1 disables it).
 *
 * ---------------------------------------------------------------------------
 * The two-granule problem
 *
 * Stage 2 (IPA -> host, via hv_vm_map) is fixed at 16KiB by the framework, on
 * host address, IPA and size alike. Stage 1 (VA -> IPA, these tables) is ours,
 * and is 4KiB because that is what aarch64 Linux userland expects.
 *
 * So a guest 4KiB mapping cannot be one hv_vm_map call. Instead:
 *
 *   - Guest physical space is carved into 16KiB stage-2 chunks, each backed by
 *     one 16KiB host allocation and mapped once with hv_vm_map.
 *   - A 4KiB guest page is a level-3 descriptor pointing at a 4KiB-aligned IPA
 *     *inside* one of those chunks.
 *
 * The guest therefore only ever sees the pages it was actually given, even
 * though the host mapped memory around them. Rounding stage 2 up per guest page
 * instead would publish the three neighbouring 4KiB pages into the guest's
 * address space, which is an isolation bug rather than an accounting one.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "noah.h"
#include "mm.h"
#include "vmm.h"
#include "page.h"
#include "arm64/vm.h"
#include "linux/mman.h"

/*
 * A 16KiB stage-2 chunk: one host allocation, one hv_vm_map, four 4KiB guest
 * pages' worth of room.
 */
struct s2_chunk {
  void    *hva;
  gaddr_t  ipa;
  unsigned used;      /* 4KiB slots handed out, 0..PAGES_PER_CHUNK */
};

#define PAGES_PER_CHUNK (STAGE2_GRANULE / PAGE_SIZEOF(PAGE_4KB))   /* 4 */

/*
 * Guest-physical allocation cursor. IPAs are ours to assign - nothing outside
 * the guest observes them - so they are handed out linearly from a base chosen
 * to stay clear of the low addresses a guest might use as a null-ish sentinel.
 */
#define IPA_BASE 0x40000000UL

static gaddr_t   ipa_brk = IPA_BASE;
static struct s2_chunk cur_chunk;

/*
 * Every chunk handed out, so an IPA can be turned back into a host pointer.
 *
 * The tree's guest_to_host() walks the mm_region bookkeeping in src/mm/mm.c,
 * which does not know about these chunks - they are guest-physical space this
 * allocator owns, below the mm layer. Table walks need the reverse lookup
 * (a descriptor holds an IPA; writing the next level needs the host address),
 * so keep it here.
 *
 * A flat array is deliberate. Lookups happen only while building tables, never
 * on a fault path, and the count is small; a tree would be more code for no
 * measurable gain.
 */
#define MAX_CHUNKS 4096
static struct s2_chunk chunks[MAX_CHUNKS];
static size_t nr_chunks;

static void *
ipa_to_host(gaddr_t ipa)
{
  for (size_t i = 0; i < nr_chunks; i++) {
    if (ipa >= chunks[i].ipa && ipa < chunks[i].ipa + STAGE2_GRANULE)
      return (char *) chunks[i].hva + (ipa - chunks[i].ipa);
  }
  panic("no stage-2 chunk backs IPA 0x%llx", (unsigned long long) ipa);
  return NULL;
}

/*
 * Hand out one 4KiB slot of guest-physical space, backed by host memory.
 *
 * Chunks are filled before a new one is allocated, so four consecutive 4KiB
 * requests cost one hv_vm_map rather than four.
 */
static void *
alloc_guest_page(gaddr_t *ipa_out)
{
  if (cur_chunk.hva == NULL || cur_chunk.used == PAGES_PER_CHUNK) {
    void *hva = NULL;
    /* Host allocation must be 16KiB-aligned for hv_vm_map. On Apple Silicon
     * that is simply the host page size, but say so explicitly rather than
     * relying on it. */
    if (posix_memalign(&hva, STAGE2_GRANULE, STAGE2_GRANULE) != 0)
      panic("out of memory allocating a stage-2 chunk");
    memset(hva, 0, STAGE2_GRANULE);

    cur_chunk.hva  = hva;
    cur_chunk.ipa  = ipa_brk;
    cur_chunk.used = 0;
    ipa_brk += STAGE2_GRANULE;

    vmm_mmap(cur_chunk.ipa, STAGE2_GRANULE,
             HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC, cur_chunk.hva);

    if (nr_chunks == MAX_CHUNKS)
      panic("stage-2 chunk table full");
    chunks[nr_chunks++] = cur_chunk;
  }

  size_t off = (size_t) cur_chunk.used * PAGE_SIZEOF(PAGE_4KB);
  cur_chunk.used++;

  *ipa_out = cur_chunk.ipa + off;
  return (char *) cur_chunk.hva + off;
}

/* ------------------------------------------------------------- tables */

/*
 * Translation tables are themselves guest pages: the hardware walks them
 * through stage 2, so they need an IPA, not just a host address. Each table is
 * one 4KiB page of 512 descriptors, so it fits a single slot from
 * alloc_guest_page.
 */
struct table {
  uint64_t *entries;   /* host view  */
  gaddr_t   ipa;       /* guest view, what a descriptor points at */
};

static struct table l1_table;

static struct table
alloc_table(void)
{
  struct table t;
  gaddr_t ipa;
  t.entries = alloc_guest_page(&ipa);
  t.ipa = ipa;
  /* alloc_guest_page zeroes the whole chunk on first use, but a later slot in
   * a partially used chunk is only zero because nothing wrote it. Be explicit;
   * a stray non-zero descriptor is a fault that is very hard to read. */
  memset(t.entries, 0, PAGE_SIZEOF(PAGE_4KB));
  return t;
}

#define L1_INDEX(va) (((va) >> PAGE_SHIFTOF(PAGE_1GB)) & (NR_PAGE_ENTRY - 1))
#define L2_INDEX(va) (((va) >> PAGE_SHIFTOF(PAGE_2MB)) & (NR_PAGE_ENTRY - 1))
#define L3_INDEX(va) (((va) >> PAGE_SHIFTOF(PAGE_4KB)) & (NR_PAGE_ENTRY - 1))

/* The output address field of a descriptor is bits [47:12]. */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

/*
 * Walk to the level-3 descriptor for `va`, creating intermediate tables as
 * needed. Returns a host pointer to the descriptor slot.
 */
static uint64_t *
walk_to_l3(gaddr_t va)
{
  assert(l1_table.entries != NULL);

  uint64_t *l1e = &l1_table.entries[L1_INDEX(va)];
  if ((*l1e & PTE_VALID) == 0) {
    struct table l2 = alloc_table();
    *l1e = (l2.ipa & PTE_ADDR_MASK) | PTE_TABLE | PTE_VALID;
  }
  uint64_t *l2_entries = ipa_to_host(*l1e & PTE_ADDR_MASK);

  uint64_t *l2e = &l2_entries[L2_INDEX(va)];
  if ((*l2e & PTE_VALID) == 0) {
    struct table l3 = alloc_table();
    *l2e = (l3.ipa & PTE_ADDR_MASK) | PTE_TABLE | PTE_VALID;
  }
  uint64_t *l3_entries = ipa_to_host(*l2e & PTE_ADDR_MASK);

  return &l3_entries[L3_INDEX(va)];
}

/*
 * Translate Linux mprot bits into descriptor permission bits.
 *
 * AP is inverted relative to the x86 PTE_W/PTE_U scheme and easy to get wrong:
 * AP[1] (PTE_AP_RW_EL0) *grants* EL0 access, and AP[2] (PTE_AP_RO) *removes*
 * write. Execute is likewise negative - UXN and PXN are execute-NEVER - so an
 * executable page is one where they are clear.
 *
 * PXN is set unconditionally: the guest's code runs at EL0 and nothing should
 * ever be executable at EL1 except the trampoline, which is mapped separately.
 */
static uint64_t
prot_to_pte(int prot)
{
  uint64_t pte = PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_IDX_NORMAL) |
                 PTE_AP_RW_EL0 | PTE_PXN;

  if ((prot & LINUX_PROT_WRITE) == 0)
    pte |= PTE_AP_RO;
  if ((prot & LINUX_PROT_EXEC) == 0)
    pte |= PTE_UXN;

  return pte;
}

/*
 * Map one 4KiB guest virtual page onto a 4KiB guest-physical page.
 *
 * The access flag is set eagerly (PTE_AF). Hardware AF management is optional
 * on ARMv8 and a descriptor without it faults on first touch; since nothing
 * here uses AF for page aging, setting it up front avoids a fault that would
 * have to be handled for no benefit.
 */
void
pt_map_page(gaddr_t va, gaddr_t ipa, int prot)
{
  assert((va & (PAGE_SIZEOF(PAGE_4KB) - 1)) == 0);
  assert((ipa & (PAGE_SIZEOF(PAGE_4KB) - 1)) == 0);

  uint64_t *l3e = walk_to_l3(va);
  *l3e = (ipa & PTE_ADDR_MASK) | prot_to_pte(prot) | PTE_PAGE | PTE_VALID;
}

/*
 * Allocate a fresh 4KiB guest page and map it at `va`. Returns the host
 * pointer, so the caller can populate it.
 */
void *
pt_alloc_and_map(gaddr_t va, int prot)
{
  gaddr_t ipa;
  void *hva = alloc_guest_page(&ipa);
  pt_map_page(va, ipa, prot);
  return hva;
}

/*
 * Map the EL1 vector page.
 *
 * The trampoline is the one thing that runs at EL1, so it is the one page that
 * must have PXN *clear* - the opposite of every other mapping, which sets PXN
 * unconditionally so nothing but the trampoline is ever privileged-executable.
 * It is also made inaccessible to EL0 (AP[1] clear, UXN set): the guest has no
 * business reading or executing its own exception vector.
 *
 * Separate from pt_map_page rather than a prot flag, because "executable at
 * EL1" is not a Linux mprot bit and pretending it is would put an
 * architecture-specific concept into the generic mapping path.
 */
void *
pt_map_vector(gaddr_t va)
{
  gaddr_t ipa;
  void *hva = alloc_guest_page(&ipa);

  uint64_t pte = PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_IDX_NORMAL) |
                 PTE_UXN;   /* EL1-exec (PXN clear), no EL0 access, no EL0 exec */

  uint64_t *l3e = walk_to_l3(va);
  *l3e = (ipa & PTE_ADDR_MASK) | pte | PTE_PAGE | PTE_VALID;
  return hva;
}

/*
 * Build the empty level-1 table and point TTBR0_EL1 at it.
 *
 * Does NOT enable the MMU: SCTLR_EL1.M stays clear until the caller has mapped
 * enough for the guest to run, because turning translation on with an empty
 * table means the very next instruction fetch faults.
 */
void
pt_init(void)
{
  ipa_brk = IPA_BASE;
  nr_chunks = 0;
  memset(&cur_chunk, 0, sizeof cur_chunk);

  l1_table = alloc_table();

  vmm_arm64_write_sysreg(HV_SYS_REG_MAIR_EL1, MAIR_EL1_VALUE);
  vmm_arm64_write_sysreg(HV_SYS_REG_TCR_EL1, TCR_EL1_VALUE);
  vmm_arm64_write_sysreg(HV_SYS_REG_TTBR0_EL1, l1_table.ipa);
}

/*
 * Turn stage-1 translation on.
 *
 * Separate from pt_init so the caller controls the moment: everything the guest
 * touches next - its code, its stack, the trampoline - must already be mapped,
 * or the first fetch after this faults with no way to make progress.
 */
void
pt_enable(void)
{
  uint64_t sctlr;
  vmm_arm64_read_sysreg(HV_SYS_REG_SCTLR_EL1, &sctlr);
  vmm_arm64_write_sysreg(HV_SYS_REG_SCTLR_EL1, sctlr | SCTLR_EL1_M);
}

gaddr_t
pt_root_ipa(void)
{
  return l1_table.ipa;
}
