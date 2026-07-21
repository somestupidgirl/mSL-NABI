/*
 * aarch64 signal frame construction.
 *
 * STUB. The neutral signal management in src/ipc/signal.c - masks, the pending
 * set, the sigaction table and all the rt_sig* syscalls - works on arm64 as-is;
 * only the two functions here, which build and restore the arch-specific signal
 * frame, are unimplemented. They are what a guest needs to actually *run* a
 * signal handler, so a program that installs a handler and receives a signal
 * will hit this; a program that never takes a delivered signal will not.
 *
 * The real implementation is a self-contained follow-up. It differs from x86
 * (signal_x86.c) in three ways with no shared code:
 *
 *   - The sigcontext is { fault_address, regs[31], sp, pc, pstate } followed by
 *     a chain of _aarch64_ctx records in a __reserved[] area - fpsimd_context
 *     (V0-V31, FPCR, FPSR), optionally esr_context, terminated by a null
 *     record - not the sixteen named GPRs plus segments x86 uses.
 *   - There is no SA_RESTORER on aarch64. The kernel points x30 at
 *     __kernel_rt_sigreturn in the vDSO, so NABI must map a small guest page
 *     holding `mov x8, #139; svc #0` and point x30 at it on delivery.
 *   - The handler is called with the signal in x0, siginfo in x1 and ucontext
 *     in x2 (x86 uses rdi/rsi/rdx).
 *
 * See PORTING-arm64.md section 3.4.
 */

#include "common.h"
#include "linux/signal.h"
#include "noah.h"
#include "vmm.h"

int
arch_setup_sigframe(int signum)
{
  (void) signum;
  panic("aarch64 signal delivery is not implemented yet: a guest installed a "
        "handler and received a signal. See signal_arm64.c / PORTING-arm64.md "
        "3.4.");
  return -LINUX_EFAULT;   /* unreached */
}

uint64_t
arch_rt_sigreturn(void)
{
  panic("aarch64 rt_sigreturn is not implemented yet. See signal_arm64.c.");
  return 0;   /* unreached */
}
