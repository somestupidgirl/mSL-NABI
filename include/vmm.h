#ifndef NOAH_VMM_H
#define NOAH_VMM_H

/*
 * The VMM interface, split by architecture.
 *
 * Everything below the "common" line is architecture-neutral and is what the
 * rest of the tree should be reaching for; see include/arch.h for the vCPU
 * interface proper. The per-architecture blocks exist because the two
 * Hypervisor.framework APIs share no vocabulary - hv_vmx.h is entirely inside
 * `#ifdef __x86_64__` and hv_vcpu.h's ARM half inside `#ifdef __arm64__`, so
 * there is nothing to unify at this level even in principle.
 */

#include "types.h"
#include "noah.h"
#include "arch.h"

#ifdef __x86_64__

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <Hypervisor/hv_arch_vmx.h>

#include "x86/vmx.h"

struct vcpu_snapshot {
  uint64_t vcpu_reg[NR_X86_REG_LIST];
  uint64_t vmcs[NR_VMCS_FIELD_MASKED];
  char fpu_states[2496] __attribute__((aligned(16)));
};

void vmm_read_register(hv_x86_reg_t, uint64_t *);
void vmm_write_register(hv_x86_reg_t, uint64_t);
void vmm_read_msr(uint32_t, uint64_t *);
void vmm_write_msr(uint32_t, uint64_t);
void vmm_read_vmcs(uint32_t, uint64_t *);
void vmm_write_vmcs(uint32_t, uint64_t);

void vmm_write_fpstate(void *, size_t);
void vmm_enable_native_msr(uint32_t, bool);

#elif defined(__arm64__)

#include <Hypervisor/Hypervisor.h>

/*
 * Provisional. The x86 snapshot is a register list, a masked VMCS field list
 * and a 2496-byte fxsave area; the aarch64 equivalent is x0-x30, SP, PC,
 * PSTATE, TPIDR_EL0 and the FPSIMD file. Fleshing that out belongs with the
 * fork/signal work in Phase 4 - see PORTING-arm64.md - so for now this carries
 * only what the backend itself touches.
 */
struct vcpu_snapshot {
  uint64_t x[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  uint64_t tpidr_el0;
};

hv_vcpu_exit_t *vmm_arm64_exit_record(void);

void vmm_arm64_read_reg(hv_reg_t, uint64_t *);
void vmm_arm64_read_simd(hv_simd_fp_reg_t, void *out16);
void vmm_arm64_write_simd(hv_simd_fp_reg_t, const void *in16);
void vmm_arm64_write_reg(hv_reg_t, uint64_t);
void vmm_arm64_read_sysreg(hv_sys_reg_t, uint64_t *);
void vmm_arm64_write_sysreg(hv_sys_reg_t, uint64_t);

void vmm_arm64_sync_guest_code(void *hva, size_t len);
void vmm_arm64_map_stage2(gaddr_t ipa, size_t size, int prot, void *haddr);
void vmm_arm64_unmap_stage2(gaddr_t ipa, size_t size);
void vmm_arm64_install_trampoline(void *hva, gaddr_t ipa);
void vmm_arm64_init_vcpu(void);
void vmm_arm64_enter_el0(gaddr_t pc, gaddr_t sp, gaddr_t el1_eret_stub);

#else
#error "unsupported architecture"
#endif

/* ------------------------------------------------------------- common */

struct vmm_snapshot {
  struct vcpu_snapshot first_vcpu_snapshot;
};

void vmm_create(void);
void vmm_destroy(void);
void vmm_snapshot(struct vmm_snapshot*);
void vmm_reentry(struct vmm_snapshot*);
void vmm_snapshot_vcpu(struct vcpu_snapshot*);
void vmm_restore_vcpu(struct vcpu_snapshot*);

void vmm_create_vcpu(struct vcpu_snapshot *);
void vmm_destroy_vcpu(void);

int vmm_enter(void);

/* prot is obtained by or'ing HV_MEMORY_READ, HV_MEMORY_EXEC, HV_MEMORY_WRITE */
void vmm_mmap(gaddr_t addr, size_t len, int prot, void *ptr);
void vmm_munmap(gaddr_t addr, size_t len);

#endif
