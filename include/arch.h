#ifndef NOAH_ARCH_H
#define NOAH_ARCH_H

/*
 * Architecture-neutral vCPU interface.
 *
 * Everything above this line - main_loop, the syscall dispatcher - is written
 * against these types and never mentions VMX, VMCS fields, x86 registers, or
 * ESR_EL2. The backends are lib/vmm_x86.c (VT-x, today) and lib/vmm_arm64.c
 * (the ARM Hypervisor API, Phase 2). See PORTING-arm64.md.
 *
 * The point of the split is that the two architectures do not merely name
 * things differently, they trap differently: x86 gets syscalls as #UD vmexits
 * because EFER.SCE is never set, while arm64 has to bounce svc off an EL1
 * trampoline (proved in spike/arm64-trap/). Normalising at the exit boundary is
 * what keeps that difference out of the syscall layer.
 */

#include <stdint.h>
#include "types.h"

/*
 * Registers, named by role rather than by architecture.
 *
 * The syscall ABIs map onto these as:
 *
 *              x86-64                aarch64
 *   SYSNR      rax                   x8
 *   ARG0..5    rdi rsi rdx r10 r8 r9  x0 x1 x2 x3 x4 x5
 *   RET        rax                   x0
 *
 * Note SYSNR and RET are the same register on x86 and different on aarch64,
 * which is why they are separate names here even though the x86 backend maps
 * both to rax.
 */
enum vreg {
  VREG_PC,
  VREG_SP,
  VREG_SYSNR,
  VREG_RET,
  VREG_ARG0,
  VREG_ARG1,
  VREG_ARG2,
  VREG_ARG3,
  VREG_ARG4,
  VREG_ARG5,
  NR_VREG,
};

/*
 * Why the guest stopped.
 *
 * Anything the backend can handle by itself - x86 CPUID, an XCR0 fixup for
 * AVX-on-demand, a host interrupt - is resolved inside the backend and never
 * produces an exit here. Those are architecture implementation details with no
 * meaning to the syscall layer, and on arm64 most of them simply do not exist.
 */
enum vm_exit_kind {
  EXIT_SYSCALL,     /* guest issued a system call                             */
  EXIT_MMU_FAULT,   /* guest touched an address that needs attention          */
  EXIT_FAULT,       /* guest took a fault we translate into a signal          */
  EXIT_RESUME,      /* backend dealt with it, or there was nothing to deal
                       with; go round again                                   */
  EXIT_UNHANDLED,   /* backend does not know what this is - diagnostic only   */
};

/* Access that caused an EXIT_MMU_FAULT. Mirrors the VERIFY_* values so the
 * fault path can hand it straight to addr_ok(). */
enum vm_access {
  VM_ACCESS_UNKNOWN,
  VM_ACCESS_READ,
  VM_ACCESS_WRITE,
  VM_ACCESS_EXEC,
};

struct vm_exit {
  enum vm_exit_kind kind;

  /* EXIT_MMU_FAULT */
  gaddr_t        fault_addr;      /* guest linear address, if the hardware
                                     reported one */
  bool           fault_addr_valid;
  enum vm_access fault_access;

  /* EXIT_FAULT: the signal the guest has earned (LINUX_SIG*). */
  int            signal;

  /* EXIT_UNHANDLED: raw, architecture-specific, for diagnostics only. Never
   * interpret this above the backend. */
  uint64_t       raw_reason;
  uint64_t       raw_qualification;
};

/*
 * Enter the guest once and describe why it stopped, in architecture-neutral
 * terms. Returns 0 on a normal exit, -1 if the vCPU could not be entered.
 *
 * Deliberately does NOT loop internally, even for exits it fully resolves
 * itself (CPUID, the x86 AVX/XCR0 fixup, a host interrupt). Those return
 * EXIT_RESUME so that re-entry goes back through the caller's loop, which
 * checks for pending signals first. Retrying inside the backend would skip
 * that check and delay signal delivery for a guest taking frequent exits.
 */
int vmm_run(struct vm_exit *);

void vmm_get_reg(enum vreg, uint64_t *);
void vmm_set_reg(enum vreg, uint64_t);

/*
 * Advance the program counter past the instruction that caused an
 * EXIT_SYSCALL, if the architecture requires it.
 *
 * x86 does: the #UD leaves RIP pointing AT the two-byte `syscall`, so the host
 * has to step over it. aarch64 does not: the CPU has already advanced ELR_EL1
 * past the `svc` by the time the host sees the trap - measured, see
 * PORTING-arm64.md section 4, Phase 0 - so the arm64 backend implements this as
 * a no-op. Doing it there anyway would silently skip an instruction after every
 * single syscall.
 *
 * Must be called AFTER the syscall handler has run, because execve rewrites the
 * program counter and the advance has to apply to the new value.
 */
void vmm_syscall_return(void);

#endif
