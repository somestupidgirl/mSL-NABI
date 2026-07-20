/*
 * Tests for the VT-x exit decoder (lib/vmm_x86_exit.c).
 *
 * Why this exists: the x86 build cannot be executed on Apple Silicon at all -
 * hv_vm_create() returns HV_UNSUPPORTED under Rosetta - so test/test.rb needs
 * real Intel hardware and Phase 1 landed without a runnable regression check.
 * Decoding is the part of the backend that can still be tested anywhere,
 * because it reaches the hardware only through the accessors in vmm.h. This
 * file substitutes those with a fake machine and asserts the architecture-
 * neutral struct vm_exit that comes out.
 *
 * The binary is x86_64 and runs under Rosetta. It never creates a VM.
 *
 * What this does NOT cover: guest execution, the syscall handlers, the
 * register file against real hardware. It covers the mapping from a vmexit to
 * a struct vm_exit, which is exactly what Phase 1 rewrote.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vmm.h"
#include "arch.h"
#include "x86/vm.h"
#include "x86/vmx.h"
#include "x86/irq_vectors.h"
#include "x86/specialreg.h"
#include "linux/signal.h"

/* ===================================================================== fakes */

#define VMCS_TABLE_SIZE 0x8000
#define REG_TABLE_SIZE  64
#define GUEST_MEM_BASE  0x400000UL
#define GUEST_MEM_SIZE  256

static uint64_t fake_vmcs[VMCS_TABLE_SIZE];
static uint64_t fake_regs[REG_TABLE_SIZE];
static uint8_t  guest_mem[GUEST_MEM_SIZE];
static bool     guest_mem_readable;

int
vmm_enter(void)
{
  return 0;   /* the guest "ran"; the fake VMCS already describes the exit */
}

void
vmm_read_vmcs(uint32_t field, uint64_t *val)
{
  assert(field < VMCS_TABLE_SIZE);
  *val = fake_vmcs[field];
}

void
vmm_write_vmcs(uint32_t field, uint64_t val)
{
  assert(field < VMCS_TABLE_SIZE);
  fake_vmcs[field] = val;
}

void
vmm_read_register(hv_x86_reg_t reg, uint64_t *val)
{
  assert((unsigned) reg < REG_TABLE_SIZE);
  *val = fake_regs[reg];
}

void
vmm_write_register(hv_x86_reg_t reg, uint64_t val)
{
  assert((unsigned) reg < REG_TABLE_SIZE);
  fake_regs[reg] = val;
}

size_t
copy_from_user(void *haddr, gaddr_t gaddr, size_t n)
{
  if (!guest_mem_readable)
    return n;                       /* non-zero means failure */
  if (gaddr < GUEST_MEM_BASE || gaddr + n > GUEST_MEM_BASE + GUEST_MEM_SIZE)
    return n;
  memcpy(haddr, guest_mem + (gaddr - GUEST_MEM_BASE), n);
  return 0;
}

/* The decoder logs freely; keep the test output readable. */
void printk(const char *fmt, ...) { (void) fmt; }
void warnk(const char *fmt, ...) { (void) fmt; }

/* ================================================================= scaffolding */

static int failures;
static int checks;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("  FAIL: ");                                                      \
      printf(__VA_ARGS__);                                                     \
      printf("\n        (%s:%d: %s)\n", __FILE__, __LINE__, #cond);            \
    }                                                                          \
  } while (0)

static const char *
kind_name(enum vm_exit_kind k)
{
  switch (k) {
  case EXIT_SYSCALL:   return "EXIT_SYSCALL";
  case EXIT_MMU_FAULT: return "EXIT_MMU_FAULT";
  case EXIT_FAULT:     return "EXIT_FAULT";
  case EXIT_RESUME:    return "EXIT_RESUME";
  case EXIT_UNHANDLED: return "EXIT_UNHANDLED";
  }
  return "?";
}

static void
reset(void)
{
  memset(fake_vmcs, 0, sizeof fake_vmcs);
  memset(fake_regs, 0, sizeof fake_regs);
  memset(guest_mem, 0, sizeof guest_mem);
  guest_mem_readable = true;
  fake_regs[HV_X86_RIP] = GUEST_MEM_BASE;
}

/* Build an interrupt-info field: vector in [7:0], type in [10:8]. */
static uint64_t
exc_info(int type, int vector)
{
  return ((uint64_t) type << 8) | (uint64_t) vector;
}

static void
put_guest_code(const void *bytes, size_t n)
{
  memcpy(guest_mem, bytes, n);
}

/* ====================================================================== cases */

static void
test_external_interrupt_resumes(void)
{
  printf("external interrupt / NMI -> EXIT_RESUME\n");

  int types[] = { VMCS_EXCTYPE_EXTERNAL_INTERRUPT,
                  VMCS_EXCTYPE_NONMASKTABLE_INTERRUPT };
  for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
    reset();
    fake_vmcs[VMCS_RO_EXIT_REASON]      = VMX_REASON_EXC_NMI;
    fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]  = exc_info(types[i], 0);

    struct vm_exit e;
    CHECK(vmm_run(&e) == 0, "vmm_run failed");
    CHECK(e.kind == EXIT_RESUME, "type %d gave %s, want EXIT_RESUME",
          types[i], kind_name(e.kind));
  }
}

static void
test_syscall_detected(void)
{
  printf("#UD on a two-byte 0f 05 -> EXIT_SYSCALL\n");

  reset();
  const uint8_t syscall_insn[] = { 0x0f, 0x05 };
  put_guest_code(syscall_insn, sizeof syscall_insn);
  fake_vmcs[VMCS_RO_EXIT_REASON]       = VMX_REASON_EXC_NMI;
  fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]   = exc_info(VMCS_EXCTYPE_HARDWARE_EXCEPTION,
                                                  X86_VEC_UD);
  fake_vmcs[VMCS_RO_VMEXIT_INSTR_LEN]  = 2;

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_SYSCALL, "got %s, want EXIT_SYSCALL", kind_name(e.kind));

  /* The decoder must NOT advance the PC itself - main_loop does that after the
   * handler, via vmm_syscall_return(), because execve rewrites it. */
  CHECK(fake_regs[HV_X86_RIP] == GUEST_MEM_BASE,
        "decoder advanced RIP to 0x%llx; it must leave that to "
        "vmm_syscall_return()", fake_regs[HV_X86_RIP]);
}

static void
test_bad_opcode_is_sigill(void)
{
  printf("#UD on something else -> EXIT_FAULT / SIGILL\n");

  reset();
  const uint8_t junk[] = { 0xde, 0xad, 0xbe, 0xef };
  put_guest_code(junk, sizeof junk);
  fake_vmcs[VMCS_RO_EXIT_REASON]      = VMX_REASON_EXC_NMI;
  fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]  = exc_info(VMCS_EXCTYPE_HARDWARE_EXCEPTION,
                                                 X86_VEC_UD);
  fake_vmcs[VMCS_RO_VMEXIT_INSTR_LEN] = 4;

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_FAULT, "got %s, want EXIT_FAULT", kind_name(e.kind));
  CHECK(e.signal == LINUX_SIGILL, "signal %d, want SIGILL(%d)",
        e.signal, LINUX_SIGILL);
}

static void
test_avx_already_enabled_is_sigill(void)
{
  printf("VEX prefix with AVX already on in XCR0 -> EXIT_FAULT / SIGILL\n");

  /* The other AVX path (enable-and-retry) depends on the host's CPUID leaf
   * 0x0d, which is emulated under Rosetta, so it is not deterministic enough to
   * assert here. This branch is: if XCR0 already advertises AVX there is
   * nothing to fix up, so it must fall through to the invalid-opcode path. */
  reset();
  const uint8_t vex[] = { 0xc5, 0xf8, 0x77 };   /* vzeroupper */
  put_guest_code(vex, sizeof vex);
  fake_regs[HV_X86_XCR0]              = XCR0_AVX_STATE;
  fake_vmcs[VMCS_RO_EXIT_REASON]      = VMX_REASON_EXC_NMI;
  fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]  = exc_info(VMCS_EXCTYPE_HARDWARE_EXCEPTION,
                                                 X86_VEC_UD);
  fake_vmcs[VMCS_RO_VMEXIT_INSTR_LEN] = 3;

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_FAULT, "got %s, want EXIT_FAULT", kind_name(e.kind));
  CHECK(e.signal == LINUX_SIGILL, "signal %d, want SIGILL", e.signal);
}

static void
test_page_fault_does_not_fall_through(void)
{
  printf("#PF -> EXIT_FAULT / SIGSEGV, and does NOT fall into the #UD path\n");

  /*
   * Regression test for the missing `break` fixed in Phase 1. The original
   * fell from X86_VEC_PF straight into X86_VEC_UD, so a page fault sent
   * SIGSEGV and then ALSO ran the invalid-opcode handler.
   *
   * The guest memory here holds a valid two-byte `syscall` and instlen is 2,
   * so if the fallthrough ever comes back this returns EXIT_SYSCALL instead of
   * EXIT_FAULT and the test fails loudly.
   */
  reset();
  const uint8_t syscall_insn[] = { 0x0f, 0x05 };
  put_guest_code(syscall_insn, sizeof syscall_insn);
  fake_vmcs[VMCS_RO_EXIT_REASON]      = VMX_REASON_EXC_NMI;
  fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]  = exc_info(VMCS_EXCTYPE_HARDWARE_EXCEPTION,
                                                 X86_VEC_PF);
  fake_vmcs[VMCS_RO_VMEXIT_INSTR_LEN] = 2;
  fake_vmcs[VMCS_RO_EXIT_QUALIFIC]    = 0xdeadb000;

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_FAULT, "got %s, want EXIT_FAULT (fallthrough is back?)",
        kind_name(e.kind));
  CHECK(e.signal == LINUX_SIGSEGV, "signal %d, want SIGSEGV(%d)",
        e.signal, LINUX_SIGSEGV);
}

static void
test_ept_violation_access_kinds(void)
{
  printf("EPT violation -> EXIT_MMU_FAULT with the right access kind\n");

  struct { uint64_t qual_bit; enum vm_access want; const char *name; } cases[] = {
    { 1u << 0, VM_ACCESS_READ,  "read"  },
    { 1u << 1, VM_ACCESS_WRITE, "write" },
    { 1u << 2, VM_ACCESS_EXEC,  "exec"  },
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    reset();
    fake_vmcs[VMCS_RO_EXIT_REASON]       = VMX_REASON_EPT_VIOLATION;
    /* bit 7 = the guest linear address field is valid */
    fake_vmcs[VMCS_RO_EXIT_QUALIFIC]     = (1u << 7) | cases[i].qual_bit;
    fake_vmcs[VMCS_RO_GUEST_LIN_ADDR]    = 0xcafe000;

    struct vm_exit e;
    CHECK(vmm_run(&e) == 0, "vmm_run failed");
    CHECK(e.kind == EXIT_MMU_FAULT, "%s: got %s, want EXIT_MMU_FAULT",
          cases[i].name, kind_name(e.kind));
    CHECK(e.fault_addr_valid, "%s: fault_addr_valid not set", cases[i].name);
    CHECK(e.fault_addr == 0xcafe000, "%s: fault_addr 0x%llx, want 0xcafe000",
          cases[i].name, (unsigned long long) e.fault_addr);
    CHECK(e.fault_access == cases[i].want, "%s: wrong access kind",
          cases[i].name);
  }
}

static void
test_ept_violation_without_linear_address(void)
{
  printf("EPT violation without a valid linear address -> not actionable\n");

  reset();
  fake_vmcs[VMCS_RO_EXIT_REASON]    = VMX_REASON_EPT_VIOLATION;
  fake_vmcs[VMCS_RO_EXIT_QUALIFIC]  = 0;      /* bit 7 clear */
  fake_vmcs[VMCS_RO_GUEST_LIN_ADDR] = 0xbad;  /* must be ignored */

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_MMU_FAULT, "got %s, want EXIT_MMU_FAULT",
        kind_name(e.kind));
  CHECK(!e.fault_addr_valid,
        "fault_addr_valid set even though qualification bit 7 was clear - "
        "main_loop would check a bogus address");
}

static void
test_cpuid_handled_internally(void)
{
  printf("CPUID -> handled in the backend, EXIT_RESUME, RIP advanced\n");

  reset();
  fake_vmcs[VMCS_RO_EXIT_REASON] = VMX_REASON_CPUID;
  fake_regs[HV_X86_RAX] = 0;

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_RESUME, "got %s, want EXIT_RESUME", kind_name(e.kind));
  CHECK(fake_regs[HV_X86_RIP] == GUEST_MEM_BASE + 2,
        "RIP 0x%llx, want +2 past the cpuid", fake_regs[HV_X86_RIP]);
}

static void
test_irq_and_hlt_resume(void)
{
  printf("IRQ / HLT -> EXIT_RESUME\n");

  uint64_t reasons[] = { VMX_REASON_IRQ, VMX_REASON_HLT };
  for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
    reset();
    fake_vmcs[VMCS_RO_EXIT_REASON] = reasons[i];

    struct vm_exit e;
    CHECK(vmm_run(&e) == 0, "vmm_run failed");
    CHECK(e.kind == EXIT_RESUME, "reason %llu gave %s, want EXIT_RESUME",
          (unsigned long long) reasons[i], kind_name(e.kind));
  }
}

static void
test_unknown_reason(void)
{
  printf("unrecognised exit reason -> EXIT_UNHANDLED, raw reason preserved\n");

  reset();
  fake_vmcs[VMCS_RO_EXIT_REASON] = 0x7f;   /* not a reason the decoder knows */

  struct vm_exit e;
  CHECK(vmm_run(&e) == 0, "vmm_run failed");
  CHECK(e.kind == EXIT_UNHANDLED, "got %s, want EXIT_UNHANDLED",
        kind_name(e.kind));
  CHECK(e.raw_reason == 0x7f, "raw_reason 0x%llx, want 0x7f",
        (unsigned long long) e.raw_reason);
}

static void
test_register_mapping(void)
{
  printf("vreg mapping matches the x86-64 syscall ABI\n");

  reset();

  /* Distinct values so a mis-wired entry cannot pass by coincidence. */
  fake_regs[HV_X86_RAX] = 0x1001;
  fake_regs[HV_X86_RDI] = 0x2002;
  fake_regs[HV_X86_RSI] = 0x3003;
  fake_regs[HV_X86_RDX] = 0x4004;
  fake_regs[HV_X86_R10] = 0x5005;
  fake_regs[HV_X86_RCX] = 0xbadbad;   /* must NOT be arg3 */
  fake_regs[HV_X86_R8]  = 0x6006;
  fake_regs[HV_X86_R9]  = 0x7007;
  fake_regs[HV_X86_RSP] = 0x8008;

  struct { enum vreg r; uint64_t want; const char *name; } m[] = {
    { VREG_SYSNR, 0x1001, "SYSNR -> rax" },
    { VREG_RET,   0x1001, "RET   -> rax" },
    { VREG_ARG0,  0x2002, "ARG0  -> rdi" },
    { VREG_ARG1,  0x3003, "ARG1  -> rsi" },
    { VREG_ARG2,  0x4004, "ARG2  -> rdx" },
    { VREG_ARG3,  0x5005, "ARG3  -> r10, NOT rcx" },
    { VREG_ARG4,  0x6006, "ARG4  -> r8"  },
    { VREG_ARG5,  0x7007, "ARG5  -> r9"  },
    { VREG_SP,    0x8008, "SP    -> rsp" },
    { VREG_PC, GUEST_MEM_BASE, "PC -> rip" },
  };

  for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
    uint64_t got = 0;
    vmm_get_reg(m[i].r, &got);
    CHECK(got == m[i].want, "%s: got 0x%llx want 0x%llx", m[i].name,
          (unsigned long long) got, (unsigned long long) m[i].want);
  }

  /* The return value goes back through rax. */
  vmm_set_reg(VREG_RET, 0xfeed);
  CHECK(fake_regs[HV_X86_RAX] == 0xfeed, "VREG_RET did not write rax");
}

static void
test_syscall_return_advances_pc(void)
{
  printf("vmm_syscall_return() steps over the two-byte syscall\n");

  reset();
  fake_regs[HV_X86_RIP] = 0x1234;
  vmm_syscall_return();
  CHECK(fake_regs[HV_X86_RIP] == 0x1236,
        "RIP 0x%llx, want 0x1236", fake_regs[HV_X86_RIP]);

  /* Re-reads RIP rather than caching it, so that execve - which replaces the
   * PC during the handler - still gets the advance applied to the new value. */
  fake_regs[HV_X86_RIP] = 0xABC000;
  vmm_syscall_return();
  CHECK(fake_regs[HV_X86_RIP] == 0xABC002,
        "RIP 0x%llx after a handler moved it, want 0xABC002",
        fake_regs[HV_X86_RIP]);
}

static void
test_unhandled_vector_exits(void)
{
  printf("an unhandled exception vector still terminates the process\n");

  /*
   * This path calls exit(1), which is deliberate (see the comment in
   * vmm_x86_exit.c), so it is exercised in a child and checked by status
   * rather than in-process.
   */
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    reset();
    fake_vmcs[VMCS_RO_EXIT_REASON]      = VMX_REASON_EXC_NMI;
    fake_vmcs[VMCS_RO_VMEXIT_IRQ_INFO]  = exc_info(VMCS_EXCTYPE_HARDWARE_EXCEPTION,
                                                   X86_VEC_GP);
    fake_vmcs[VMCS_RO_VMEXIT_INSTR_LEN] = 2;
    struct vm_exit e;
    vmm_run(&e);
    _exit(0);          /* reached only if the decoder returned instead */
  }

  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
        "expected exit(1) on an unhandled vector, got status 0x%x", status);
}

int
main(void)
{
  /* The decoder dumps offending instruction bytes straight to stderr, by
   * design. Assertions report on stdout, so drop stderr to keep the output
   * readable. */
  freopen("/dev/null", "w", stderr);

  printf("== VT-x exit decoder ==\n\n");

  test_external_interrupt_resumes();
  test_syscall_detected();
  test_bad_opcode_is_sigill();
  test_avx_already_enabled_is_sigill();
  test_page_fault_does_not_fall_through();
  test_ept_violation_access_kinds();
  test_ept_violation_without_linear_address();
  test_cpuid_handled_internally();
  test_irq_and_hlt_resume();
  test_unknown_reason();
  test_register_mapping();
  test_syscall_return_advances_pc();
  test_unhandled_vector_exits();

  printf("\n%d checks, %d failures\n", checks, failures);
  if (failures == 0)
    printf("PASS\n");
  else
    printf("FAIL\n");
  return failures != 0;
}
