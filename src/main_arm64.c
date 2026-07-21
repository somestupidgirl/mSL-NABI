/*
 * aarch64 guest machine setup: page tables, the vCPU control registers and the
 * EL1 trampoline, plus the startup transition into EL0.
 *
 * The counterpart of main_x86.c, and much smaller - there is no VMCS, no
 * segments, no IDT, no MSR passthrough. The pieces here are the same ones the
 * hardware tests already exercise (pt_init, vmm_arm64_init_vcpu, the
 * trampoline, pt_enable, enter_el0); this file wires them into the
 * init_vkernel_machine / vmm_start_guest interface main.c drives.
 */

#include <stdint.h>

#include "common.h"
#include "vmm.h"
#include "mm.h"
#include "noah.h"
#include "page.h"
#include "arm64/vm.h"

/* From src/mm/pt_arm64.c */
void  pt_init(void);
void  pt_enable(void);
void *pt_map_vector(gaddr_t va);

/*
 * The EL1 trampoline sits at the base of kernel space, just above the user
 * address range, well clear of anything a guest binary maps. 2KiB-aligned for
 * VBAR_EL1, which user_addr_max (page-aligned) satisfies.
 */
#define GUEST_TRAMPOLINE_VA (user_addr_max)

static gaddr_t tramp_va;

void
init_vkernel_machine(void)
{
  /* Page tables (TTBR0/TCR/MAIR), MMU still off. */
  pt_init();

  /* CPACR (FP/SIMD at EL0), MAIR/TCR. */
  vmm_arm64_init_vcpu();

  /* Map and install the EL1 trampoline, plus its one-shot startup eret. */
  void *hva = pt_map_vector(GUEST_TRAMPOLINE_VA);
  vmm_arm64_install_trampoline(hva, GUEST_TRAMPOLINE_VA);
  ((uint32_t *) hva)[VEC_BOOT_OFF / sizeof(uint32_t)] = INSN_ERET;
  vmm_arm64_sync_guest_code(hva, PAGE_SIZEOF(PAGE_4KB));

  tramp_va = GUEST_TRAMPOLINE_VA;
}

void
vmm_start_guest(void)
{
  /* do_exec set the entry point and stack via VREG_PC / VREG_SP. */
  uint64_t pc = 0, sp = 0;
  vmm_get_reg(VREG_PC, &pc);
  vmm_get_reg(VREG_SP, &sp);

  /* Everything the guest touches next - its code, stack and this trampoline -
   * is mapped, so translation can go on; then eret from EL1 down to EL0 at the
   * entry point. */
  pt_enable();
  vmm_arm64_enter_el0(pc, sp, tramp_va + VEC_BOOT_OFF);
}
