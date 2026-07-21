/*
 * aarch64 guest machine memory setup.
 *
 * The counterpart of src/mm/mm_x86.c, and much smaller. ARM has no
 * segmentation, so init_segment has nothing to do; and the guest page tables
 * are not a static identity map but the stage-1 tables built in
 * src/mm/pt_arm64.c, so init_page delegates there.
 *
 * NOTE: not yet wired into a running init sequence. main.c's init_vkernel is
 * still x86-only (it writes VMCS fields directly), so nothing calls these yet.
 * They exist so the arm64 machine-setup has the same shape as x86, and so the
 * eventual arm64 main.c has these symbols to call. See PORTING-arm64.md.
 */

#include "mm.h"
#include "vmm.h"
#include "noah.h"

/* From src/mm/pt_arm64.c */
void pt_init(void);

/*
 * Build the guest's stage-1 translation tables and point TTBR0_EL1 at them.
 *
 * Does NOT enable the MMU - pt_init leaves SCTLR_EL1.M clear - because the
 * guest's code, stack and the trampoline must all be mapped first. Turning
 * translation on is pt_enable(), called later once the address space is
 * populated. This mirrors x86 init_page writing CR3 without the guest yet
 * running.
 */
void
init_page(void)
{
  pt_init();
}

/*
 * No segmentation on aarch64. Privilege is the exception level and the vector
 * table is a page of code (the EL1 trampoline), not a descriptor table, so
 * there is nothing here that init_segment does on x86.
 *
 * Kept as an empty function rather than deleted so the init sequence can call
 * it unconditionally regardless of architecture.
 */
void
init_segment(void)
{
}
