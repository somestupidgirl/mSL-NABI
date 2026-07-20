/*
 * VT-x exit decoding: the architecture-neutral interface from include/arch.h.
 *
 * Split from lib/vmm_x86.c, which keeps the Hypervisor.framework plumbing (VM
 * and vCPU lifecycle, the raw register/VMCS/MSR accessors, snapshots). The
 * split exists so this half can be tested without VT-x: everything here reaches
 * the hardware only through the accessors declared in vmm.h, so a test can
 * substitute them. See test/arch/.
 *
 * That matters because the x86 build cannot be executed on Apple Silicon at all
 * - hv_vm_create() returns HV_UNSUPPORTED under Rosetta - so decoding is the
 * only part of this backend that can be checked on the development machine.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cpuid.h>

#include "vmm.h"
#include "arch.h"
#include "mm.h"
#include "x86/vm.h"
#include "x86/vmx.h"
#include "x86/irq_vectors.h"
#include "x86/specialreg.h"
#include "linux/signal.h"

/* Moved from src/main.c: the exit decoder below is now its only caller,
 * and it is entirely VMCS-specific. */
#define get_bit(integer, n) (int)((integer & ( 1 << n )) >> n)

#define GET_VMCS(val, var) \
        var = 0;\
        vmm_read_vmcs(val, &var);

#define GET_MSR(val, var) \
        var = 0;\
        vmm_read_msr(val, &var);

static void check_vm_entry()
{
        uint64_t tmp;
        uint64_t controls, pin_based, cpu_based1, cpu_based2;
        uint64_t cr0, cr4;
        uint8_t unrestricted_guest, load_debug_controls, ia_32e_mode_guest,
                ia_32_perf_global_ctrl, ia_32_pat, ia_32_efer, ia_32_bndcfgs;


        GET_VMCS(VMCS_CTRL_VMENTRY_CONTROLS, controls);
        GET_VMCS(VMCS_CTRL_PIN_BASED, pin_based);
        GET_VMCS(VMCS_CTRL_CPU_BASED, cpu_based1);
        GET_VMCS(VMCS_CTRL_CPU_BASED2, cpu_based2);

        unrestricted_guest      = get_bit(cpu_based2, 7);
        load_debug_controls     = get_bit(controls, 2);
        ia_32e_mode_guest       = get_bit(controls, 9);
        ia_32_perf_global_ctrl  = get_bit(controls, 13);
        ia_32_pat               = get_bit(controls, 14);
        ia_32_efer              = get_bit(controls, 15);
        ia_32_bndcfgs           = get_bit(controls, 16);

        GET_VMCS(VMCS_GUEST_CR0, cr0);
        GET_VMCS(VMCS_GUEST_CR4, cr4);

        if (!unrestricted_guest) {
                assert(!get_bit(cr0, 31) || get_bit(cr0, 0));
        }

        if (load_debug_controls) {
                GET_VMCS(VMCS_GUEST_IA32_DEBUGCTL, tmp);
                vmm_write_vmcs(VMCS_GUEST_IA32_DEBUGCTL, tmp & 0b1101111111000011);
                GET_VMCS(VMCS_GUEST_IA32_DEBUGCTL, tmp);
                assert(!get_bit(tmp, 2)
                    && !get_bit(tmp, 3)
                    && !get_bit(tmp, 4)
                    && !get_bit(tmp, 5)
                    && !get_bit(tmp, 13)
                    && tmp < 65535);
        }

        if (ia_32e_mode_guest) {
                assert(get_bit(cr0, 31) && get_bit(cr4, 5));
        } else {
                assert(!get_bit(cr4, 17));
        }

        GET_VMCS(VMCS_GUEST_CR3, tmp);
        assert(!tmp); // CR3 field must be such that bits 63:52 and
                      // bits in the range 51:32 beyond the
                      // processor's physical address width are 0

        if (load_debug_controls) {
                GET_VMCS(VMCS_GUEST_DR7, tmp);
                assert(tmp < 0b100000000000000000000000000000000);
        }

        warnk("Didn't check IA32_SYSENTER_ESP canonical\n");
        warnk("Didn't check IA32_SYSENTER_EIP canonical\n");


        if (ia_32_perf_global_ctrl) {
                warnk("IA_32_PERF_GLOBAL_CTRL not tested\n");
                GET_VMCS(VMCS_GUEST_IA32_PERF_GLOBAL_CTRL, tmp);
                assert(!tmp); // Too few bits not reserved
        }

        if (ia_32_pat) {
                warnk("IA_32_PAT not tested\n");
                GET_VMCS(VMCS_GUEST_IA32_PAT, tmp);
                for (int i = 0; i < 8; ++i) {
                        char tmpbyte = tmp & 0xff;
                        assert(tmpbyte == 0
                                        || tmpbyte == 1
                                        || tmpbyte == 4
                                        || tmpbyte == 5
                                        || tmpbyte == 6
                                        || tmpbyte == 7);
                        tmp >>= 8;

                }
        }

        if (ia_32_efer) {
                GET_VMCS(VMCS_GUEST_IA32_EFER, tmp);
                assert(!tmp); // Too few bits not reserved
                assert(get_bit(tmp, 10) == ia_32e_mode_guest);
                assert(!get_bit(cr0, 31) || (get_bit(tmp, 10) == get_bit(tmp, 8)));
        }

        if (ia_32_bndcfgs) {
                 warnk("Didn't check IA32_BNDCFGS\n");
        }

        printk("EVERYTHING CLEAR SO FAR\n");

}
/* ==========================================================================
 * Architecture-neutral interface (include/arch.h)
 *
 * Everything below normalises VT-x into the vocabulary main_loop speaks. The
 * x86 vmexit reasons that have no meaning above this line - CPUID, the
 * XCR0 fixup for AVX-on-demand - are handled here and the guest is resumed
 * without troubling the caller.
 * ========================================================================== */

/*
 * VREG_SYSNR and VREG_RET are both rax: on x86-64 the syscall number arrives
 * in the same register the result goes back in. aarch64 splits them (x8 and
 * x0), which is why arch.h keeps them apart.
 */
static const hv_x86_reg_t vreg_map[NR_VREG] = {
  [VREG_PC]    = HV_X86_RIP,
  [VREG_SP]    = HV_X86_RSP,
  [VREG_SYSNR] = HV_X86_RAX,
  [VREG_RET]   = HV_X86_RAX,
  [VREG_ARG0]  = HV_X86_RDI,
  [VREG_ARG1]  = HV_X86_RSI,
  [VREG_ARG2]  = HV_X86_RDX,
  /* r10, not rcx: the syscall instruction clobbers rcx with the return
   * address, so the kernel ABI substitutes r10 for the fourth argument. */
  [VREG_ARG3]  = HV_X86_R10,
  [VREG_ARG4]  = HV_X86_R8,
  [VREG_ARG5]  = HV_X86_R9,
};

void
vmm_get_reg(enum vreg reg, uint64_t *val)
{
  assert(reg < NR_VREG);
  vmm_read_register(vreg_map[reg], val);
}

void
vmm_set_reg(enum vreg reg, uint64_t val)
{
  assert(reg < NR_VREG);
  vmm_write_register(vreg_map[reg], val);
}

void
vmm_syscall_return(void)
{
  /* Step over the two-byte `syscall`. The #UD leaves RIP pointing at it, so
   * without this the guest would re-execute the same syscall forever.
   *
   * RIP is re-read rather than cached from before the handler ran, because
   * execve replaces it wholesale and the advance must apply to the new value.
   */
  uint64_t rip;
  vmm_read_register(HV_X86_RIP, &rip);
  vmm_write_register(HV_X86_RIP, rip + 2);
}

static bool
is_avx(int instlen, uint64_t rip)
{
  uint8_t op;
  if (copy_from_user(&op, rip, sizeof op))
    return false;
  return op == 0xc4 || op == 0xc5;
}

static bool
is_syscall(int instlen, uint64_t rip)
{
  static const ushort OP_SYSCALL = 0x050f;
  if (instlen != 2)
    return false;
  ushort op;
  if (copy_from_user(&op, rip, sizeof op))
    return false;
  return op == OP_SYSCALL;
}

/* Report an unhandled exception as a signal, after dumping the offending
 * instruction bytes - which is the only useful thing to say about it. */
static void
report_bad_instruction(struct vm_exit *vmexit, uint64_t rip, uint64_t instlen,
                       const char *what, int sig)
{
  warnk("%s (rip = %p): ", what, (void *) rip);
  unsigned char inst[instlen];
  if (copy_from_user(inst, rip, instlen)) {
    fprintf(stderr, "<unreadable>\n");
  } else {
    for (uint64_t i = 0; i < instlen; ++i)
      fprintf(stderr, "%02x ", inst[i] & 0xff);
    fprintf(stderr, "\n");
  }
  vmexit->kind = EXIT_FAULT;
  vmexit->signal = sig;
}

int
vmm_run(struct vm_exit *vmexit)
{
  memset(vmexit, 0, sizeof *vmexit);

  if (vmm_enter() < 0)
    return -1;

  uint64_t exit_reason;
  vmm_read_vmcs(VMCS_RO_EXIT_REASON, &exit_reason);
  vmexit->raw_reason = exit_reason;

  switch (exit_reason) {
  case VMX_REASON_VMCALL:
    printk("reason: vmcall\n");
    assert(false);
    vmexit->kind = EXIT_UNHANDLED;
    return 0;

  case VMX_REASON_EXC_NMI: {
    /* Intel SDM 27.2.2, Table 24-15: Information for VM Exits Due to
     * Vectored Events */
    uint64_t exc_info;
    vmm_read_vmcs(VMCS_RO_VMEXIT_IRQ_INFO, &exc_info);

    int int_type = (exc_info & 0x700) >> 8;
    switch (int_type) {
    default:
      assert(false);
    case VMCS_EXCTYPE_EXTERNAL_INTERRUPT:
    case VMCS_EXCTYPE_NONMASKTABLE_INTERRUPT:
      /* Nothing we can do; the host OS handles it. */
      vmexit->kind = EXIT_RESUME;
      return 0;
    case VMCS_EXCTYPE_HARDWARE_EXCEPTION: /* including invalid opcode */
    case VMCS_EXCTYPE_SOFTWARE_EXCEPTION: /* including breakpoints, overflows */
      break;
    }

    uint64_t instlen, rip;
    vmm_read_vmcs(VMCS_RO_VMEXIT_INSTR_LEN, &instlen);
    vmm_read_register(HV_X86_RIP, &rip);

    int exc_vec = exc_info & 0xff;
    switch (exc_vec) {
    case X86_VEC_PF: {
      uint64_t gladdr;
      vmm_read_vmcs(VMCS_RO_EXIT_QUALIFIC, &gladdr);
      printk("page fault: caused by guest linear address 0x%llx\n", gladdr);
      vmexit->kind = EXIT_FAULT;
      vmexit->signal = LINUX_SIGSEGV;
      return 0;
    }

    case X86_VEC_UD:
      /*
       * The syscall path. EFER.SCE is never set, so the guest's `syscall`
       * faults as an invalid opcode instead of executing, and that #UD is
       * how NABI gets control. There is no arm64 equivalent - see
       * PORTING-arm64.md section 2.
       */
      if (is_syscall(instlen, rip)) {
        vmexit->kind = EXIT_SYSCALL;
        return 0;
      }
      /*
       * AVX-on-demand: the guest used a VEX-encoded instruction while
       * XCR0's AVX bit was clear. Enable it and retry. Purely an x86
       * concern, so it is resolved here rather than surfacing an exit.
       */
      if (is_avx(instlen, rip)) {
        uint64_t xcr0;
        vmm_read_register(HV_X86_XCR0, &xcr0);
        if ((xcr0 & XCR0_AVX_STATE) == 0) {
          unsigned int eax, ebx, ecx, edx;
          __cpuid_count(0x0d, 0x0, eax, ebx, ecx, edx);
          if (eax & XCR0_AVX_STATE) {
            vmm_write_register(HV_X86_XCR0, xcr0 | XCR0_AVX_STATE);
            vmexit->kind = EXIT_RESUME;
            return 0;
          }
        }
      }
      report_bad_instruction(vmexit, rip, instlen, "invalid opcode!",
                             LINUX_SIGILL);
      return 0;

    /*
     * Every other vector - DE, DB, BP, OF, BR, NM, DF, TS, NP, SS, GP, MF,
     * AC, MC, XM, VE, SX. The original enumerated all seventeen and then let
     * them fall into this same default, so listing them bought nothing.
     *
     * Still a hard exit(1) rather than a signal, deliberately. It carries a
     * FIXME/TODO upstream and converting it to SIGILL would be a plausible
     * improvement, but a guest that limps on after an unhandled machine
     * exception is much harder to debug than one that stops - and during the
     * port, an unexpected stop is exactly the signal worth keeping.
     */
    default:
      warnk("exception thrown: %d\n", exc_vec);
      fprintf(stderr, "inst: \n");
      unsigned char inst[instlen];
      if (copy_from_user(inst, rip, instlen))
        assert(false);
      for (uint64_t i = 0; i < instlen; ++i)
        fprintf(stderr, "%02x ", inst[i] & 0xff);
      fprintf(stderr, "\n");
      exit(1);
    }
  }

  case VMX_REASON_EPT_VIOLATION: {
    printk("reason: ept_violation\n");

    uint64_t gpaddr;
    vmm_read_vmcs(VMCS_GUEST_PHYSICAL_ADDRESS, &gpaddr);
    printk("guest-physical address = 0x%llx\n", gpaddr);

    uint64_t qual;
    vmm_read_vmcs(VMCS_RO_EXIT_QUALIFIC, &qual);
    printk("exit qualification = 0x%llx\n", qual);

    vmexit->kind = EXIT_MMU_FAULT;
    vmexit->raw_qualification = qual;

    /* Bit 7: the guest linear address field is valid. Without it there is
     * nothing to check, and the old code just logged and resumed. */
    if (qual & (1 << 7)) {
      uint64_t gladdr;
      vmm_read_vmcs(VMCS_RO_GUEST_LIN_ADDR, &gladdr);
      printk("guest linear address = 0x%llx\n", gladdr);

      vmexit->fault_addr = gladdr;
      vmexit->fault_addr_valid = true;

      if (qual & (1 << 0))
        vmexit->fault_access = VM_ACCESS_READ;
      else if (qual & (1 << 1))
        vmexit->fault_access = VM_ACCESS_WRITE;
      else if (qual & (1 << 2))
        vmexit->fault_access = VM_ACCESS_EXEC;
    } else {
      printk("guest linear address = (unavailable)\n");
    }
    return 0;
  }

  /*
   * CPUID is emulated by forwarding to the host's, then stepping over the
   * two-byte instruction. Entirely an x86 concern: ARM ID registers are read
   * with mrs and only trap if configured to, so this has no arm64 analogue.
   */
  case VMX_REASON_CPUID: {
    uint64_t rax;
    vmm_read_register(HV_X86_RAX, &rax);
    unsigned eax, ebx, ecx, edx;
    __get_cpuid(rax, &eax, &ebx, &ecx, &edx);

    vmm_write_register(HV_X86_RAX, eax);
    vmm_write_register(HV_X86_RBX, ebx);
    vmm_write_register(HV_X86_RCX, ecx);
    vmm_write_register(HV_X86_RDX, edx);

    uint64_t rip;
    vmm_read_register(HV_X86_RIP, &rip);
    vmm_write_register(HV_X86_RIP, rip + 2);
    vmexit->kind = EXIT_RESUME;
    return 0;
  }

  case VMX_REASON_IRQ:
  case VMX_REASON_HLT:
    vmexit->kind = EXIT_RESUME;
    return 0;

  default: {
    /*
     * Intel SDM Volume 3B, section 21.9: VM-exit information fields.
     * Bit 31 of the exit reason marks a VM-entry failure.
     */
    uint64_t qual;
    if (exit_reason & (1u << 31)) {
      printk("VM-entry failure exit reason: %llx\n", exit_reason ^ (1u << 31));
    } else {
      printk("other exit reason: %llx\n", exit_reason);
    }
    if (exit_reason & VMX_REASON_VMENTRY_GUEST)
      check_vm_entry();
    vmm_read_vmcs(VMCS_RO_EXIT_QUALIFIC, &qual);
    printk("exit qualification: %llx\n", qual);

    vmexit->kind = EXIT_UNHANDLED;
    vmexit->raw_qualification = qual;
    return 0;
  }
  }

  /* Every case above returns. */
  __builtin_unreachable();
}
