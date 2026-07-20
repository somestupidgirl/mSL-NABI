#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <cpuid.h>
#include <getopt.h>
#include <string.h>
#include <sys/syslimits.h>
#include <libgen.h>
#include <strings.h>
#include <fcntl.h>

#include "common.h"
#include "vmm.h"
#include "arch.h"
#include "mm.h"
#include "noah.h"
#include "syscall.h"
#include "linux/errno.h"
#include "x86/irq_vectors.h"
#include "x86/specialreg.h"
#include "x86/vm.h"
#include "x86/vmx.h"
#include <sys/sysctl.h>

#include <mach-o/dyld.h>

static int
get_cpuid_count (unsigned int leaf,
		 unsigned int subleaf,
		 unsigned int *eax, unsigned int *ebx,
		 unsigned int *ecx, unsigned int *edx)
{
    __cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
    return 1;
}

static int
handle_syscall(void)
{
  uint64_t nr;
  vmm_get_reg(VREG_SYSNR, &nr);
  if (nr >= NR_SYSCALLS) {
    warnk("unknown system call: %lld\n", nr);
    send_signal(getpid(), LINUX_SIGSYS);
  }
  uint64_t a0, a1, a2, a3, a4, a5;
  vmm_get_reg(VREG_ARG0, &a0);
  vmm_get_reg(VREG_ARG1, &a1);
  vmm_get_reg(VREG_ARG2, &a2);
  vmm_get_reg(VREG_ARG3, &a3);
  vmm_get_reg(VREG_ARG4, &a4);
  vmm_get_reg(VREG_ARG5, &a5);
  uint64_t retval = sc_handler_table[nr](a0, a1, a2, a3, a4, a5);
  vmm_set_reg(VREG_RET, retval);

  if (nr == LSYS_rt_sigreturn) {
    return -1;
  }
  return 0;
}
/*
 * Run the guest once, having first delivered anything pending.
 *
 * Returns 0 on a normal exit with *exit filled in, -1 if the vCPU could not be
 * entered.
 */
static int
task_run(struct vm_exit *exit)
{
  /* handle pending signals */
  if (has_sigpending()) {
    handle_signal();
  }
  return vmm_run(exit);
}

void
main_loop(int return_on_sigret)
{
  /* main_loop returns only if return_on_sigret == 1 && rt_sigreturn is invoked.
     see also: rt_sigsuspend */

  struct vm_exit exit;

  while (task_run(&exit) == 0) {

    /* dump_instr(); */
    /* print_regs(); */

    switch (exit.kind) {
    case EXIT_SYSCALL: {
      int r = handle_syscall();
      /* After the handler, never before: execve rewrites the program counter
       * and the advance has to apply to the new value. On aarch64 this is a
       * no-op - the CPU has already stepped past the svc. */
      vmm_syscall_return();
      if (return_on_sigret && r < 0) {
        return;
      }
      break;
    }

    case EXIT_MMU_FAULT:
      if (exit.fault_addr_valid) {
        int verify = 0;
        switch (exit.fault_access) {
        case VM_ACCESS_READ:    verify = VERIFY_READ;  break;
        case VM_ACCESS_WRITE:   verify = VERIFY_WRITE; break;
        case VM_ACCESS_EXEC:    verify = VERIFY_EXEC;  break;
        case VM_ACCESS_UNKNOWN: break;
        }
        if (!addr_ok(exit.fault_addr, verify)) {
          printk("page fault: caused by guest linear address 0x%llx\n",
                 exit.fault_addr);
          send_signal(getpid(), LINUX_SIGSEGV);
        }
      }
      break;

    case EXIT_FAULT:
      send_signal(getpid(), exit.signal);
      break;

    case EXIT_RESUME:
      /* Backend handled it, or there was nothing to handle. */
      break;

    case EXIT_UNHANDLED:
      /* The backend has already logged whatever it could work out. */
      break;
    }
  }

  __builtin_unreachable();
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

static void
init_first_proc(const char *root)
{
  proc = (struct proc) {
    .nr_tasks = 1,
    .lock = PTHREAD_RWLOCK_INITIALIZER,
    .mm = malloc(sizeof(struct mm)),
  };
  INIT_LIST_HEAD(&proc.tasks);
  list_add(&task.head, &proc.tasks);
  init_mm(proc.mm);
  init_signal();
  int rootfd = open(root, O_RDONLY | O_DIRECTORY);
  if (rootfd < 0) {
    perror("could not open initial root directory");
    exit(1);
  }
  init_fileinfo(rootfd);
  close(rootfd);
  proc.pfutex = kh_init(pfutex);
  pthread_mutex_init(&proc.futex_mutex, NULL);
  proc.cred = (struct cred) {
    .lock = PTHREAD_RWLOCK_INITIALIZER,
    .uid = getuid(),
    .euid = geteuid(),
    .suid = geteuid(),
  };

  task.tid = getpid();
}

static void
init_vkernel(const char *root)
{
  init_mm(&vkern_mm);
  init_shm_malloc();
  init_vmcs();
  init_msr();
  init_page();
  init_special_regs();
  init_segment();
  init_idt();
  init_regs();
  init_fpu();

  init_first_proc(root);
}

void
drop_privilege(void)
{
  if (seteuid(getuid()) != 0) {
    panic("drop_privilege");
  }
}

int sys_setresuid(int, int, int);
void
elevate_privilege(void)
{
  pthread_rwlock_wrlock(&proc.cred.lock);
  proc.cred.euid = 0;
  proc.cred.suid = 0;
  if (seteuid(0) != 0) {
    panic("elevate_privilege");
  }
  pthread_rwlock_unlock(&proc.cred.lock);
}

noreturn void
die_with_forcedsig(int sig)
{
  // TODO: Termination processing

  /* Force default signal action */
  int dsig = linux_to_darwin_signal(sig);
  sigset_t mask;
  sigfillset(&mask);
  sigdelset(&mask, dsig);
  sigprocmask(SIG_SETMASK, &mask, NULL);
  struct sigaction act;
  act.sa_handler = SIG_DFL;
  act.sa_flags = 0;
  sigaction(dsig, &act, NULL);
  raise(dsig);
  assert(false); // sig should be one that can terminate procs
}

void
check_platform_version(void)
{
  int32_t b;
  size_t len = sizeof b;

  if (sysctlbyname("kern.hv_support", &b, &len, NULL, 0) < 0) {
    perror("sysctl kern.hv_support");
  }
  if (b == 0) {
    fprintf(stderr, "Your cpu seems too old. Buy a new mac!\n");
    exit(1);
  }
}

int
main(int argc, char *argv[], char **envp)
{
  drop_privilege();

  check_platform_version();

  char root[PATH_MAX] = {};

  int c;
  enum {PRINTK_PATH, WARNK_PATH, STRACE_PATH, MAX_DEBUG_PATH};
  char debug_paths[3][PATH_MAX] = {};
  struct option long_options[] = {
    { "output", required_argument, NULL, 'o'},
    { "strace", required_argument, NULL, 's'},
    { "warning", required_argument, NULL, 'w'},
    { "mnt", required_argument, NULL, 'm' },
    { "help", no_argument, NULL, 'h' },
    { 0, 0, 0, 0 }
  };

  while ((c = getopt_long(argc, argv, "+ho:w:s:m:", long_options, NULL)) != -1) {
    switch (c) {
    case 'o':
      strncpy(debug_paths[PRINTK_PATH], optarg, PATH_MAX);
      break;
    case 'w':
      strncpy(debug_paths[WARNK_PATH], optarg, PATH_MAX);
      break;
    case 's':
      strncpy(debug_paths[STRACE_PATH], optarg, PATH_MAX);
      break;
    case 'm':
      if (realpath(optarg, root) == NULL) {
        perror("Invalid --mnt flag: ");
        exit(1);
      }
      argv[optind - 1] = root;
      break;
    case 'h':
    default:
      printf("Usage: nabi -h | [-o output] [-w warning] [-s strace] -m /virtual/filesystem/root executable ...\n");
      exit(0);
    }
  }

  argc -= optind;
  argv += optind;

  if (argc == 0) {
    abort();
  }

  vmm_create();

  init_vkernel(root);

  for (int i = PRINTK_PATH; i < MAX_DEBUG_PATH; i++) {
    static void (* init_funcs[3])(const char *path) = {
      [PRINTK_PATH] = init_printk,
      [STRACE_PATH] = init_meta_strace,
      [WARNK_PATH]  = init_warnk
    };
    if (debug_paths[i][0] != '\0') {
      init_funcs[i](debug_paths[i]);
    }
  }

  int err;
  if ((err = do_exec(argv[0], argc, argv, envp)) < 0) {
    errno = linux_to_darwin_errno(-err);
    perror("Error");
    exit(1);
  }

  main_loop(0);

  vmm_destroy();

  return 0;
}
