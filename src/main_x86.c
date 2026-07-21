/*
 * x86-64 guest machine setup: the VMCS control fields, special registers, IDT,
 * initial register/FPU state and MSR passthrough.
 *
 * Split out of src/main.c the same way mm_x86.c was split out of mm.c. main.c
 * keeps the architecture-neutral main_loop and the process orchestration; this
 * file and main_arm64.c hold the per-architecture machine bring-up behind
 * init_vkernel_machine() and vmm_start_guest().
 */

#include <assert.h>
#include <stdint.h>
#include <cpuid.h>

#include "common.h"
#include "vmm.h"
#include "mm.h"
#include "noah.h"
#include "x86/irq_vectors.h"
#include "x86/specialreg.h"
#include "x86/vm.h"
#include "x86/vmx.h"

static int
get_cpuid_count (unsigned int leaf,
		 unsigned int subleaf,
		 unsigned int *eax, unsigned int *ebx,
		 unsigned int *ecx, unsigned int *edx)
{
    __cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
    return 1;
}

void
init_vmcs()
{
  uint64_t vmx_cap_pinbased, vmx_cap_procbased, vmx_cap_procbased2, vmx_cap_entry, vmx_cap_exit;

  hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &vmx_cap_pinbased);
  hv_vmx_read_capability(HV_VMX_CAP_PROCBASED, &vmx_cap_procbased);
  hv_vmx_read_capability(HV_VMX_CAP_PROCBASED2, &vmx_cap_procbased2);
  hv_vmx_read_capability(HV_VMX_CAP_ENTRY, &vmx_cap_entry);
  hv_vmx_read_capability(HV_VMX_CAP_EXIT, &vmx_cap_exit);

  /* set up vmcs misc */

#define cap2ctrl(cap,ctrl) (((ctrl) | ((cap) & 0xffffffff)) & ((cap) >> 32))

  vmm_write_vmcs(VMCS_CTRL_PIN_BASED, cap2ctrl(vmx_cap_pinbased, 0));
  vmm_write_vmcs(VMCS_CTRL_CPU_BASED, cap2ctrl(vmx_cap_procbased,
                                               CPU_BASED_HLT |
                                               CPU_BASED_CR8_LOAD |
                                               CPU_BASED_CR8_STORE));
  vmm_write_vmcs(VMCS_CTRL_CPU_BASED2, cap2ctrl(vmx_cap_procbased2, 0));
  vmm_write_vmcs(VMCS_CTRL_VMENTRY_CONTROLS,  cap2ctrl(vmx_cap_entry,
                                                       VMENTRY_LOAD_EFER |
                                                       VMENTRY_GUEST_IA32E));
  vmm_write_vmcs(VMCS_CTRL_VMEXIT_CONTROLS, cap2ctrl(vmx_cap_exit, VMEXIT_LOAD_EFER));
  vmm_write_vmcs(VMCS_CTRL_EXC_BITMAP, 0xffffffff);
  vmm_write_vmcs(VMCS_CTRL_CR0_SHADOW, 0);
  vmm_write_vmcs(VMCS_CTRL_CR4_MASK, 0);
  vmm_write_vmcs(VMCS_CTRL_CR4_SHADOW, 0);
}

void
init_special_regs()
{
  uint64_t cr0;
  vmm_read_vmcs(VMCS_GUEST_CR0, &cr0);
  vmm_write_vmcs(VMCS_GUEST_CR0, (cr0 & ~CR0_EM) | CR0_MP);

  uint64_t cr4;
  vmm_read_vmcs(VMCS_GUEST_CR4, &cr4);
  vmm_write_vmcs(VMCS_GUEST_CR4, cr4 | CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_VMXE | CR4_OSXSAVE);

  uint64_t efer;
  vmm_read_vmcs(VMCS_GUEST_IA32_EFER, &efer);
  vmm_write_vmcs(VMCS_GUEST_IA32_EFER, efer | EFER_LME | EFER_LMA);
}

struct gate_desc idt[256] __page_aligned;
gaddr_t idt_ptr;

void
init_idt()
{
  idt_ptr = kmap(idt, 0x1000, HV_MEMORY_READ | HV_MEMORY_WRITE);

  vmm_write_vmcs(VMCS_GUEST_IDTR_BASE, idt_ptr);
  vmm_write_vmcs(VMCS_GUEST_IDTR_LIMIT, sizeof idt);
}

void
init_regs()
{
  /* set up cpu regs */
  vmm_write_register(HV_X86_RFLAGS, 0x2);
  unsigned int eax, ebx, ecx, edx;
  get_cpuid_count(0x0d, 0x0, &eax, &ebx, &ecx, &edx);
  if (eax & XCR0_SSE_STATE) {
    uint64_t xcr0;
    vmm_read_register(HV_X86_XCR0, &xcr0);
    vmm_write_register(HV_X86_XCR0, xcr0 | XCR0_SSE_STATE);
  }
}

void
init_msr()
{
  vmm_enable_native_msr(MSR_TIME_STAMP_COUNTER, 1);
  vmm_enable_native_msr(MSR_TSC_AUX, 1);
  vmm_enable_native_msr(MSR_KERNEL_GS_BASE, 1);
}

void
init_fpu()
{
  struct fxregs_state {
    uint16_t cwd; /* Control Word                    */
    uint16_t swd; /* Status Word                     */
    uint16_t twd; /* Tag Word                        */
    uint16_t fop; /* Last Instruction Opcode         */
    union {
      struct {
        uint64_t rip; /* Instruction Pointer             */
        uint64_t rdp; /* Data Pointer                    */
      };
      struct {
        uint32_t fip; /* FPU IP Offset                   */
        uint32_t fcs; /* FPU IP Selector                 */
        uint32_t foo; /* FPU Operand Offset              */
        uint32_t fos; /* FPU Operand Selector            */
      };
    };
    uint32_t mxcsr;       /* MXCSR Register State */
    uint32_t mxcsr_mask;  /* MXCSR Mask           */
    uint32_t st_space[32]; /* 8*16 bytes for each FP-reg = 128 bytes */
    uint32_t xmm_space[64]; /* 16*16 bytes for each XMM-reg = 256 bytes */
    uint32_t __padding[12];
    union {
      uint32_t __padding1[12];
      uint32_t sw_reserved[12];
    };
  } __attribute__((aligned(16))) fx;

  /* emulate 'fninit'
   * - http://www.felixcloutier.com/x86/FINIT:FNINIT.html
   */
  fx.cwd = 0x037f;
  fx.swd = 0;
  fx.twd = 0xffff;
  fx.fop = 0;
  fx.rip = 0;
  fx.rdp = 0;

  /* default configuration for the SIMD core */
  fx.mxcsr = 0x1f80;
  fx.mxcsr_mask = 0;

  vmm_write_fpstate(&fx, sizeof fx);
}


/*
 * The x86 machine bring-up sequence, in the exact order main.c ran it inline.
 * init_page and init_segment live in mm_x86.c; the rest are above.
 */
void
init_vkernel_machine(void)
{
  init_vmcs();
  init_msr();
  init_page();
  init_special_regs();
  init_segment();
  init_idt();
  init_regs();
  init_fpu();
}

/*
 * Nothing to do: on x86 the guest runs with a flat identity map and no MMU-
 * enable moment, so once do_exec has set RIP it is already runnable. The arm64
 * counterpart turns on translation and drops to EL0.
 */
void
vmm_start_guest(void)
{
}
