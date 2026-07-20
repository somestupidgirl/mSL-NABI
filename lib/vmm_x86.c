#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libgen.h>
#include <sys/syslimits.h>

#include "vmm.h"
#include "mm.h"
#include "util/list.h"

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <Hypervisor/hv_arch_vmx.h>

#include <cpuid.h>

#include "x86/vm.h"
#include "x86/vmx.h"
#include "x86/irq_vectors.h"
#include "x86/specialreg.h"
#include "linux/signal.h"

struct vcpu {
  struct list_head list;
  hv_vcpuid_t vcpuid;
};

struct list_head vcpus;
int nr_vcpus;
pthread_rwlock_t alloc_lock;

_Thread_local static struct vcpu *vcpu;

void
vmm_mmap(gaddr_t gaddr, size_t size, int prot, void *haddr)
{
  assert(is_page_aligned(haddr, PAGE_4KB));
  assert(is_page_aligned((void *) gaddr, PAGE_4KB));
  assert(is_page_aligned((void *) size, PAGE_4KB));

  hv_vm_unmap(gaddr, size);
  if (hv_vm_map(haddr, gaddr, size, prot) != HV_SUCCESS) {
    panic("hv_vm_map failed\n");
  }
}

void
vmm_munmap(gaddr_t gaddr, size_t size)
{
  assert(is_page_aligned((void *) size, PAGE_4KB));
  hv_vm_unmap(gaddr, size);
}

void
vmm_write_fpstate(void *buffer, size_t size)
{
  if (hv_vcpu_write_fpstate(vcpu->vcpuid, buffer, size) != HV_SUCCESS) {
    abort();
  }
}

void
vmm_enable_native_msr(uint32_t msr, bool enable)
{
  if (hv_vcpu_enable_native_msr(vcpu->vcpuid, msr, enable) != HV_SUCCESS) {
    abort();
  }
}

void
vmm_create()
{
  hv_return_t ret;

  /* initialize global variables */
  pthread_rwlock_init(&alloc_lock, NULL);
  INIT_LIST_HEAD(&vcpus);
  nr_vcpus = 0;

  /* create the VM */
  ret = hv_vm_create(HV_VM_DEFAULT);
  if (ret != HV_SUCCESS) {
    panic("could not create the vm: error code %x", ret);
    return;
  }

  printk("successfully created the vm\n");

  vmm_create_vcpu(NULL);

  printk("successfully created a vcpu\n");
}

void
vmm_destroy()
{
  hv_return_t ret;

  struct vcpu *vcpu;
  list_for_each_entry (vcpu, &vcpus, list) {
    ret = hv_vcpu_destroy(vcpu->vcpuid);
    if (ret != HV_SUCCESS) {
      panic("could not destroy the vcpu: error code %x", ret);
      exit(1);
    }
  }

  printk("successfully destroyed the vcpu\n");

  ret = hv_vm_destroy();
  if (ret != HV_SUCCESS) {
    panic("could not destroy the vm: error code %x", ret);
    exit(1);
  }

  printk("successfully destroyed the vm\n");
}

void
vmm_create_vcpu(struct vcpu_snapshot *snapshot)
{
  hv_return_t ret;
  hv_vcpuid_t vcpuid;

  ret = hv_vcpu_create(&vcpuid, HV_VCPU_DEFAULT);
  if (ret != HV_SUCCESS) {
    panic("could not create a vcpu: error code %x", ret);
    return;
  }

  assert(vcpu == NULL);

  vcpu = calloc(sizeof(struct vcpu), 1);
  vcpu->vcpuid = vcpuid;

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

void
print_regs()
{
  uint64_t value;

  vmm_read_register(HV_X86_RIP, &value);
  printk("\trip = 0x%llx\n", value);
  vmm_read_register(HV_X86_RAX, &value);
  printk("\trax = 0x%llx\n", value);
  vmm_read_register(HV_X86_RBX, &value);
  printk("\trbx = 0x%llx\n", value);
  vmm_read_register(HV_X86_RCX, &value);
  printk("\trcx = 0x%llx\n", value);
  vmm_read_register(HV_X86_RDX, &value);
  printk("\trdx = 0x%llx\n", value);
  vmm_read_register(HV_X86_RDI, &value);
  printk("\trdi = 0x%llx\n", value);
  vmm_read_register(HV_X86_RSI, &value);
  printk("\trsi = 0x%llx\n", value);
  vmm_read_register(HV_X86_RBP, &value);
  printk("\trbp = 0x%llx\n", value);
}

void
dump_instr()
{
  uint64_t instlen, rip;
  vmm_read_vmcs(VMCS_RO_VMEXIT_INSTR_LEN, &instlen);
  vmm_read_register(HV_X86_RIP, &rip);
  char inst_str[instlen * 3 + 1];
  for (size_t i = 0; i < instlen; i ++) {
    unsigned char *ip = guest_to_host(rip);
    if (ip) {
      /* Exactly 3 bytes plus the terminator, which is why the buffer is
       * instlen*3+1. snprintf rather than sprintf: the latter is deprecated,
       * and only warns at -O0 because -O2 redirects it through the fortified
       * builtin, so the debug build was noisier than the release build. */
      snprintf(inst_str + 3 * i, sizeof inst_str - 3 * i, "%02x ", ip[i]);
    } else {
      printk("rip is in invalid user address: 0x%016llx\n", rip);
      send_signal(getpid(), LINUX_SIGSEGV);
      return;
    }
  }
  inst_str[instlen * 3] = '\0';
  printk("len: %lld, instruction: %s\n", instlen, inst_str);
}

void
vmm_snapshot_vcpu(struct vcpu_snapshot *snapshot)
{
  /* snapshot registers */
  for (uint64_t i = 0; i < NR_X86_REG_LIST; i++) {
    vmm_read_register(x86_reg_list[i], &snapshot->vcpu_reg[i]);
  }
  /* snapshot vmcs */
  for (uint64_t i = 0; i < NR_VMCS_FIELD_MASKED; i++) {
    vmm_read_vmcs(vmcs_field_masked_list[i], &snapshot->vmcs[i]);
  }
  hv_vcpu_read_fpstate(vcpu->vcpuid, snapshot->fpu_states, sizeof snapshot->fpu_states);
}

void
vmm_snapshot(struct vmm_snapshot *snapshot)
{
  printk("vmm_snapshot\n");

  pthread_rwlock_rdlock(&alloc_lock);

  if (nr_vcpus > 1) {
    fprintf(stderr, "multi-threaded fork is not implemented yet.\n");
    exit(1);
  }

  vmm_snapshot_vcpu(&snapshot->first_vcpu_snapshot);

  pthread_rwlock_unlock(&alloc_lock);
}

void init_msr(); // TODO: save and resotre MSR. just call init_msr in main.c now

void
vmm_restore_vcpu(struct vcpu_snapshot *snapshot)
{
  /* restore vmcs */
  for (uint64_t i = 0; i < NR_VMCS_FIELD_MASKED; i++) {
    vmm_write_vmcs(vmcs_field_masked_list[i], snapshot->vmcs[i]);
  }

  /* restore registers */
  for (uint64_t i = 0; i < NR_X86_REG_LIST; i++) {
    vmm_write_register(x86_reg_list[i], snapshot->vcpu_reg[i]);
  }

  /* restore fpu states */
  hv_vcpu_write_fpstate(vcpu->vcpuid, snapshot->fpu_states, sizeof snapshot->fpu_states);

  /* restore MSRs. Initializing them is enough now */
  init_msr();
}

bool
restore_ept()
{
  struct list_head *list;

  list_for_each (list, &vkern_mm.mm_regions) {
    struct mm_region *p = list_entry(list, struct mm_region, list);
    if (hv_vm_map(p->haddr, p->gaddr, p->size, linux_mprot_to_hv_mflag(p->prot)) != HV_SUCCESS)
      return false;
  }
  list_for_each (list, &proc.mm->mm_regions) {
    struct mm_region *p = list_entry(list, struct mm_region, list);
    if (hv_vm_map(p->haddr, p->gaddr, p->size, linux_mprot_to_hv_mflag(p->prot)) != HV_SUCCESS)
      return false;
  }
  return true;
}

void
vmm_reentry(struct vmm_snapshot *snapshot)
{
  hv_return_t ret;

  printk("vmm_restore\n");
  bool retried = false;
retry:
  ret = hv_vm_create(HV_VM_DEFAULT);
  if (ret != HV_SUCCESS) {
    if (!retried && ret == HV_NO_DEVICE) {
      sleep(0);
      retried = true;
      printk("retried\n");
      goto retry;
    }
    panic("could not create the vm: error code %x", ret);
    return;
  }
  printk("successfully created vm\n");

  pthread_rwlock_rdlock(&alloc_lock);

  if (nr_vcpus > 1) {
    fprintf(stderr, "multi-threaded fork is not implemented yet.\n");
    exit(1);
  }

  ret = hv_vcpu_create(&vcpu->vcpuid, HV_VCPU_DEFAULT);
  if (ret != HV_SUCCESS) {
    panic("could not create a vcpu: error code %x", ret);
    return;
  }
  vmm_restore_vcpu(&snapshot->first_vcpu_snapshot);

  pthread_rwlock_unlock(&alloc_lock);
  printk("vcpu_restore done\n");

  restore_ept();
  printk("ept_restore done\n");

}

void
vmm_read_register(hv_x86_reg_t reg, uint64_t *val)
{
  if (hv_vcpu_read_register(vcpu->vcpuid, reg, val) != HV_SUCCESS) {
    fprintf(stderr, "read_register failed\n");
    abort();
  }
}

void
vmm_write_register(hv_x86_reg_t reg, uint64_t val) {
  if (hv_vcpu_write_register(vcpu->vcpuid, reg, val) != HV_SUCCESS) {
    fprintf(stderr, "write_register failed\n");
    abort();
  }
}

void
vmm_read_msr(hv_x86_reg_t reg, uint64_t *val)
{
  if (hv_vcpu_read_msr(vcpu->vcpuid, reg, val) != HV_SUCCESS) {
    fprintf(stderr, "read_msr failed\n");
    abort();
  }
}

void
vmm_write_msr(hv_x86_reg_t reg, uint64_t val) {
  if (hv_vcpu_write_msr(vcpu->vcpuid, reg, val) != HV_SUCCESS) {
    fprintf(stderr, "write_msr failed\n");
    abort();
  }
}

void
vmm_read_vmcs(hv_x86_reg_t field, uint64_t *val)
{
  if (hv_vmx_vcpu_read_vmcs(vcpu->vcpuid, field, val) != HV_SUCCESS) {
    fprintf(stderr, "read_vmcs failed\n");
    abort();
  }
}

void
vmm_write_vmcs(hv_x86_reg_t field, uint64_t val) {
  if (hv_vmx_vcpu_write_vmcs(vcpu->vcpuid, field, val) != HV_SUCCESS) {
    /* FIXME! it fails for the VMCS_CTRL_TSC_OFFSET field on some platforms */
    //fprintf(stderr, "write_vmcs failed: %s\n", vmcs_field_to_str(field));
    //    abort();
  }
}

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

int
vmm_enter()
{
  if (hv_vcpu_run(vcpu->vcpuid) == HV_SUCCESS) {
    return 0;
  }
  return -1;
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
