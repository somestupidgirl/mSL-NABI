/*
 * Hypervisor.framework plumbing for aarch64 guests.
 *
 * The counterpart of lib/vmm_x86.c: VM and vCPU lifecycle, the raw register and
 * system-register accessors, and stage-2 mapping. Exit decoding lives next door
 * in lib/vmm_arm64_exit.c, split for the same reason as on x86 - so the decoder
 * can be tested without a VM.
 *
 * See PORTING-arm64.md. The trap design (EL0 svc -> EL1 trampoline -> hvc ->
 * host) was validated on hardware in spike/arm64-trap/ before any of this was
 * written.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vmm.h"
#include "mm.h"
#include "util/list.h"

#include <Hypervisor/Hypervisor.h>
#include <libkern/OSCacheControl.h>

#include "arm64/vm.h"

struct vcpu {
  struct list_head list;
  hv_vcpu_t vcpuid;
  hv_vcpu_exit_t *vexit;
};

struct list_head vcpus;
int nr_vcpus;
pthread_rwlock_t alloc_lock;

_Thread_local static struct vcpu *vcpu;

/* The decoder needs the exit record the framework fills in on each run. */
hv_vcpu_exit_t *
vmm_arm64_exit_record(void)
{
  return vcpu->vexit;
}

/* ------------------------------------------------------------ stage 2 */

/*
 * The stage-2 primitive: map/unmap an IPA range onto host memory. NOT the
 * arch-neutral vmm_mmap - that is the VA-region mapper in src/mm/pt_arm64.c,
 * which drives stage 1 and calls this for stage 2.
 *
 * hv_vm_map rejects anything not 16KiB-granular on host address, IPA and size
 * alike - measured, see PORTING-arm64.md section 3.5. These asserts catch that
 * up front; violating it otherwise yields HV_BAD_ARGUMENT from deep in the
 * framework, which says nothing about which of the three arguments was wrong.
 */
void
vmm_arm64_map_stage2(gaddr_t ipa, size_t size, int prot, void *haddr)
{
  assert(((uintptr_t) haddr & (STAGE2_GRANULE - 1)) == 0);
  assert((ipa & (STAGE2_GRANULE - 1)) == 0);
  assert((size & (STAGE2_GRANULE - 1)) == 0);

  hv_vm_unmap(ipa, size);
  if (hv_vm_map(haddr, ipa, size, prot) != HV_SUCCESS) {
    panic("hv_vm_map failed\n");
  }
}

void
vmm_arm64_unmap_stage2(gaddr_t ipa, size_t size)
{
  assert((size & (STAGE2_GRANULE - 1)) == 0);
  hv_vm_unmap(ipa, size);
}

/* ------------------------------------------------------- VM lifecycle */

void
vmm_create(void)
{
  hv_return_t ret;

  pthread_rwlock_init(&alloc_lock, NULL);
  INIT_LIST_HEAD(&vcpus);
  nr_vcpus = 0;

  ret = hv_vm_create(NULL);
  if (ret != HV_SUCCESS) {
    /* HV_DENIED here is almost always the entitlement, not a policy decision:
     * hv_vm_create needs com.apple.security.hypervisor and a valid signature
     * on Apple Silicon, unlike Intel where an unsigned binary could proceed. */
    panic("could not create the vm: error code %x%s", ret,
          ret == HV_DENIED ? " (missing com.apple.security.hypervisor?)" : "");
    return;
  }

  printk("successfully created the vm\n");

  vmm_create_vcpu(NULL);

  printk("successfully created a vcpu\n");
}

void
vmm_destroy(void)
{
  struct vcpu *v;
  list_for_each_entry (v, &vcpus, list) {
    if (hv_vcpu_destroy(v->vcpuid) != HV_SUCCESS) {
      panic("could not destroy the vcpu");
      exit(1);
    }
  }

  printk("successfully destroyed the vcpu\n");

  if (hv_vm_destroy() != HV_SUCCESS) {
    panic("could not destroy the vm");
    exit(1);
  }

  printk("successfully destroyed the vm\n");
}

void
vmm_create_vcpu(struct vcpu_snapshot *snapshot)
{
  hv_vcpu_t vcpuid;
  hv_vcpu_exit_t *vexit;

  /* Must be called on the thread that will run this vCPU: the framework binds
   * the vCPU to its creating thread. */
  if (hv_vcpu_create(&vcpuid, &vexit, NULL) != HV_SUCCESS) {
    panic("could not create a vcpu");
    return;
  }

  assert(vcpu == NULL);

  vcpu = calloc(1, sizeof(struct vcpu));
  vcpu->vcpuid = vcpuid;
  vcpu->vexit = vexit;

  if (snapshot) {
    vmm_restore_vcpu(snapshot);
  }

  pthread_rwlock_wrlock(&alloc_lock);
  list_add(&vcpu->list, &vcpus);
  nr_vcpus++;
  pthread_rwlock_unlock(&alloc_lock);
}

void
vmm_destroy_vcpu(void)
{
  pthread_rwlock_wrlock(&alloc_lock);
  list_del(&vcpu->list);
  nr_vcpus--;
  hv_vcpu_destroy(vcpu->vcpuid);
  free(vcpu);
  vcpu = NULL;
  pthread_rwlock_unlock(&alloc_lock);
}

/* ---------------------------------------------------------- accessors */

void
vmm_arm64_read_reg(hv_reg_t reg, uint64_t *val)
{
  if (hv_vcpu_get_reg(vcpu->vcpuid, reg, val) != HV_SUCCESS)
    panic("hv_vcpu_get_reg(%u) failed", reg);
}

void
vmm_arm64_write_reg(hv_reg_t reg, uint64_t val)
{
  if (hv_vcpu_set_reg(vcpu->vcpuid, reg, val) != HV_SUCCESS)
    panic("hv_vcpu_set_reg(%u) failed", reg);
}

void
vmm_arm64_read_sysreg(hv_sys_reg_t reg, uint64_t *val)
{
  if (hv_vcpu_get_sys_reg(vcpu->vcpuid, reg, val) != HV_SUCCESS)
    panic("hv_vcpu_get_sys_reg(%u) failed", reg);
}

void
vmm_arm64_write_sysreg(hv_sys_reg_t reg, uint64_t val)
{
  if (hv_vcpu_set_sys_reg(vcpu->vcpuid, reg, val) != HV_SUCCESS)
    panic("hv_vcpu_set_sys_reg(%u) failed", reg);
}

int
vmm_enter(void)
{
  return hv_vcpu_run(vcpu->vcpuid) == HV_SUCCESS ? 0 : -1;
}

/* Zero the FP/SIMD register file. Kept here rather than in the exit file
 * because it needs the vcpu handle, which stays private to this file. */
void
vmm_arm64_reset_fpsimd(void)
{
  const hv_simd_fp_uchar16_t zero = {0};
  for (hv_simd_fp_reg_t q = HV_SIMD_FP_REG_Q0; q <= HV_SIMD_FP_REG_Q31; q++) {
    if (hv_vcpu_set_simd_fp_reg(vcpu->vcpuid, q, zero) != HV_SUCCESS)
      panic("hv_vcpu_set_simd_fp_reg(%u) failed", q);
  }
}

/* --------------------------------------------------- code coherency */

/*
 * Make host-written bytes executable by the guest.
 *
 * Required, not advisory. Anything the host writes into guest memory that the
 * guest will then *execute* - an ELF image, the trampoline below, a signal
 * return stub - is invisible to the guest's instruction fetch until this is
 * called. Without it the guest runs whatever was at those addresses before,
 * which presents as the guest executing stale or garbage code rather than as
 * anything resembling a cache problem.
 *
 * Measured: sys_icache_invalidate alone is sufficient, sys_dcache_flush alone
 * is NOT. The stale party is the instruction path, and sys_icache_invalidate
 * issues the `dc cvau` / `ic ivau` pair that fixes it; cleaning the data cache
 * without invalidating the instruction cache changes nothing.
 *
 * x86 needs no equivalent - its caches are coherent with instruction fetch - so
 * there is no counterpart in lib/vmm_x86.c and nothing in the existing tree
 * calls anything like this. The ELF loader will have to.
 */
void
vmm_arm64_sync_guest_code(void *hva, size_t len)
{
  sys_icache_invalidate(hva, len);
}

/*
 * vmm_sync_guest_code (include/arch.h): the neutral entry the loader calls.
 * Resolves the guest virtual address to its host backing and invalidates the
 * instruction cache over it. The region must already be mapped - the loader
 * maps a segment before writing it - so guest_to_host resolves.
 */
void
vmm_sync_guest_code(gaddr_t gaddr, size_t len)
{
  void *hva = guest_to_host(gaddr);
  if (hva)
    vmm_arm64_sync_guest_code(hva, len);
}

/* -------------------------------------------------------- trampoline */

/*
 * Install the EL1 trampoline and point VBAR_EL1 at it.
 *
 * `hva` is where the host can write, `ipa` is where the guest sees it; the
 * caller has already mapped one onto the other. `ipa` must be 2KiB-aligned for
 * VBAR_EL1, which the 16KiB stage-2 granule guarantees anyway.
 *
 * The stub is deliberately two instructions and clobbers nothing. It cannot
 * afford a scratch register: on entry the guest's x0-x30 are live and a real
 * svc must preserve all of them except the return value. Discriminating svc
 * from a fault therefore happens on the host, which reads ESR_EL1 after the
 * exit - see vmm_arm64_exit.c. The alternative, saving a register to an EL1
 * stack inside the stub, would need SP_EL1 set up and is pure cost for
 * information the host can already reach.
 */
void
vmm_arm64_install_trampoline(void *hva, gaddr_t ipa)
{
  assert((ipa & (VBAR_ALIGN - 1)) == 0);

  uint32_t *vec = hva;

  /* Fill the whole table with brk so an unexpected vector is loud rather than
   * a silent walk into neighbouring code. */
  for (size_t i = 0; i < VEC_TABLE_SIZE / sizeof(uint32_t); i++)
    vec[i] = INSN_BRK0;

  uint32_t *sync = (uint32_t *)((char *) hva + VEC_LOWER64_SYNC);
  sync[0] = INSN_HVC0;   /* -> host */
  sync[1] = INSN_ERET;   /* <- host, back to EL0 via ELR_EL1/SPSR_EL1 */

  vmm_arm64_sync_guest_code(hva, VEC_TABLE_SIZE);
  vmm_arm64_write_sysreg(HV_SYS_REG_VBAR_EL1, ipa);
}

/*
 * Bring the vCPU up in a state a guest can run in.
 *
 * The MMU stays off (SCTLR_EL1.M clear) until stage-1 tables exist; with it
 * clear both EL0 and EL1 address memory flat through stage 2, which is exactly
 * what the Phase 0 spike ran in.
 *
 * CPACR_EL1 matters more than it looks: without FPEN the first FP or SIMD
 * instruction the guest executes traps as EC_SIMD_FP, and on aarch64 that is
 * essentially immediate - a plain memcpy in libc uses V registers.
 */
void
vmm_arm64_init_vcpu(void)
{
  vmm_arm64_write_sysreg(HV_SYS_REG_SCTLR_EL1, SCTLR_EL1_RES1);
  vmm_arm64_write_sysreg(HV_SYS_REG_CPACR_EL1, CPACR_EL1_FPEN_NOTRAP);
  vmm_arm64_write_sysreg(HV_SYS_REG_MAIR_EL1, MAIR_EL1_VALUE);
  vmm_arm64_write_sysreg(HV_SYS_REG_TCR_EL1, TCR_EL1_VALUE);
}

/*
 * Drop to EL0 at `pc` with stack `sp`.
 *
 * Implemented as an eret from EL1 rather than by setting CPSR to EL0t directly,
 * because that is how every subsequent return to EL0 will happen anyway - the
 * trampoline's eret uses the same two registers - so there is one path to get
 * wrong instead of two.
 */
void
vmm_arm64_enter_el0(gaddr_t pc, gaddr_t sp, gaddr_t el1_eret_stub)
{
  vmm_arm64_write_sysreg(HV_SYS_REG_SP_EL0, sp);
  vmm_arm64_write_sysreg(HV_SYS_REG_ELR_EL1, pc);
  vmm_arm64_write_sysreg(HV_SYS_REG_SPSR_EL1, PSR_EL0);
  vmm_arm64_write_reg(HV_REG_PC, el1_eret_stub);
  vmm_arm64_write_reg(HV_REG_CPSR, PSR_EL1);
}

/* ------------------------------------------------ snapshot / restore (Phase 4)
 *
 * fork and multi-threaded clone snapshot a vCPU and restore it into a fresh VM.
 * On x86 (vmm_x86.c) this walks a register list, the masked VMCS fields and the
 * fxsave area. The aarch64 equivalent - x0-x30, SP, PC, PSTATE, TPIDR_EL0 and
 * the FPSIMD file - is Phase 4, alongside the real struct vcpu_snapshot. None
 * of it is on the path to loading and running a single-threaded binary, so for
 * now these are honest stubs: fork will hit them and stop loudly rather than
 * corrupt guest state.
 */
void
vmm_snapshot_vcpu(struct vcpu_snapshot *snapshot)
{
  (void) snapshot;
  panic("vmm_snapshot_vcpu: fork/clone snapshot not implemented for arm64 yet "
        "(Phase 4). See PORTING-arm64.md.");
}

void
vmm_snapshot(struct vmm_snapshot *snapshot)
{
  (void) snapshot;
  panic("vmm_snapshot: fork not implemented for arm64 yet (Phase 4).");
}

void
vmm_restore_vcpu(struct vcpu_snapshot *snapshot)
{
  (void) snapshot;
  panic("vmm_restore_vcpu: fork/clone restore not implemented for arm64 yet "
        "(Phase 4).");
}

void
vmm_reentry(struct vmm_snapshot *snapshot)
{
  (void) snapshot;
  panic("vmm_reentry: fork not implemented for arm64 yet (Phase 4).");
}
