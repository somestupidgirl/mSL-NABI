/*
 * Phase 0 spike: can a guest `svc` be delivered to the host on Apple Silicon?
 *
 * This is the gating experiment for the whole arm64 port. See PORTING-arm64.md
 * section 2. The design under test is the EL1 trampoline:
 *
 *   1. Guest user code runs at EL0 and executes `svc #0`.
 *   2. The CPU takes that to EL1, not to the host - ARM has no equivalent of
 *      the x86 arrangement where Noah gets syscalls for free as #UD vmexits,
 *      and Hypervisor.framework exposes no HCR_EL2 bit routing svc to EL2.
 *   3. So EL1 holds a stub, pointed at by VBAR_EL1, whose only job is `hvc #0`.
 *   4. hvc DOES exit to the host: hv_vcpu_run returns HV_EXIT_REASON_EXCEPTION
 *      with ESR_EL2.EC == 0x16 (HVC64).
 *   5. The host reads the syscall number from x8, writes a result to x0, and
 *      resumes.
 *   6. The stub's `eret` returns to EL0 using the ELR_EL1/SPSR_EL1 the CPU
 *      saved on the original svc.
 *
 * If this does not work, the design is wrong and the plan needs revisiting
 * before any NABI code is touched.
 *
 * Deliberately minimal: SCTLR_EL1.M stays 0, so the MMU is off and both EL0
 * and EL1 address memory flat through stage 2. No page tables, no TTBR0_EL1,
 * no MAIR/TCR. Those are Phase 2's problem; this spike tests the trap
 * mechanism and nothing else.
 */

#include <Hypervisor/Hypervisor.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* ------------------------------------------------------------------ layout */

#define GUEST_BASE   0x10000UL      /* IPA of the mapped region; 2KiB-aligned,
                                       which VBAR_EL1 requires */
#define GUEST_SIZE   0x10000UL      /* 64KiB, a multiple of the 16KiB host page */

#define VBAR_OFF     0x000          /* vector table base                       */
#define VEC_LOWER64  0x400          /* "lower EL, AArch64, synchronous" entry   */
#define EL0_CODE_OFF 0x800          /* past the 0x800-byte vector table         */
#define EL1_BOOT_OFF 0x900          /* one-shot bootstrap that drops to EL0     */
#define STACK_OFF    0xF000         /* SP_EL0; unused here, set for safety      */

/* ------------------------------------------------------------ instructions */
/*
 * Encoded by hand rather than assembled, to keep the spike a single
 * translation unit with no build steps. MOVZ <Xd>, #imm16 is
 * 0xD2800000 | (imm16 << 5) | Rd.
 */
#define MOVZ_X8(imm) (0xD2800000u | ((uint32_t)(imm) << 5) | 8u)
#define INSN_SVC0    0xD4000001u
#define INSN_HVC0    0xD4000002u
#define INSN_ERET    0xD69F03E0u
#define INSN_BRK0    0xD4200000u

/* ------------------------------------------------------------------ PSTATE */

#define PSTATE_DAIF_MASKED 0x3C0u   /* D, A, I, F all set                      */
#define PSTATE_EL0t        (PSTATE_DAIF_MASKED | 0x0u)
#define PSTATE_EL1h        (PSTATE_DAIF_MASKED | 0x5u)

/* --------------------------------------------------------------- ESR_EL2 EC */

#define EC(syndrome) ((uint32_t)(((syndrome) >> 26) & 0x3F))
#define EC_SVC64     0x15
#define EC_HVC64     0x16
#define EC_IABT_LOW  0x20
#define EC_DABT_LOW  0x24

/* The two syscall numbers the guest issues. 64 is aarch64 `write`, 93 is
 * aarch64 `exit` - chosen so the numbers are recognisable, though nothing here
 * interprets them. */
#define SYS_FIRST  64
#define SYS_SECOND 93

/* Sentinel the host writes into x0 on the first trap. Seeing it still there at
 * the second trap is what proves the eret round-tripped back into EL0 with the
 * register file intact. */
#define SENTINEL 0xABCDULL

static const char *
ec_name(uint32_t ec)
{
	switch (ec) {
	case EC_SVC64:    return "SVC64 (svc reached the host directly?!)";
	case EC_HVC64:    return "HVC64";
	case EC_IABT_LOW: return "Instruction Abort from lower EL";
	case EC_DABT_LOW: return "Data Abort from lower EL";
	case 0x00:        return "Unknown reason";
	case 0x0E:        return "Illegal Execution State";
	case 0x3C:        return "BRK";
	default:          return "other";
	}
}

#define CHECK(expr)                                                            \
	do {                                                                   \
		hv_return_t _r = (expr);                                       \
		if (_r != HV_SUCCESS) {                                        \
			fprintf(stderr, "FAIL: %s -> 0x%x\n", #expr, _r);      \
			if (_r == HV_DENIED)                                   \
				fprintf(stderr,                                \
				    "      HV_DENIED usually means the binary " \
				    "is missing the\n"                         \
				    "      com.apple.security.hypervisor "     \
				    "entitlement, or its signature is bad.\n"); \
			exit(1);                                               \
		}                                                              \
	} while (0)

static void
put32(uint8_t *mem, size_t off, uint32_t insn)
{
	memcpy(mem + off, &insn, sizeof insn);
}

int
main(void)
{
	printf("== Phase 0: EL1 trampoline spike ==\n\n");

	/* -------------------------------------------------------- guest memory */
	void *mem = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(mem, 0, GUEST_SIZE);
	uint8_t *g = mem;

	/*
	 * EL1 vector: the whole trampoline. `svc` from EL0 lands here, and the
	 * two instructions bounce it to the host and back.
	 *
	 * A real implementation reads ESR_EL1.EC here to tell a genuine SVC
	 * (0x15) from a fault, and forwards faults through a different hvc
	 * immediate so the host can raise SIGSEGV. The spike does not need that.
	 */
	put32(g, VEC_LOWER64 + 0, INSN_HVC0);
	put32(g, VEC_LOWER64 + 4, INSN_ERET);

	/* EL0 payload: two syscalls, then a brk that should never be reached. */
	put32(g, EL0_CODE_OFF +  0, MOVZ_X8(SYS_FIRST));
	put32(g, EL0_CODE_OFF +  4, INSN_SVC0);
	put32(g, EL0_CODE_OFF +  8, MOVZ_X8(SYS_SECOND));
	put32(g, EL0_CODE_OFF + 12, INSN_SVC0);
	put32(g, EL0_CODE_OFF + 16, INSN_BRK0);

	/* One-shot EL1 bootstrap: eret into EL0 using ELR/SPSR set by the host. */
	put32(g, EL1_BOOT_OFF, INSN_ERET);

	/* ------------------------------------------------------------- the VM */
	CHECK(hv_vm_create(NULL));
	CHECK(hv_vm_map(mem, GUEST_BASE, GUEST_SIZE,
	    HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

	hv_vcpu_t vcpu;
	hv_vcpu_exit_t *vexit;
	CHECK(hv_vcpu_create(&vcpu, &vexit, NULL));

	CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, GUEST_BASE + VBAR_OFF));
	CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0,   GUEST_BASE + STACK_OFF));

	/* Start at EL1 on the bootstrap eret, which drops us to EL0. */
	CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC,   GUEST_BASE + EL1_BOOT_OFF));
	CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, PSTATE_EL1h));
	CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1,  GUEST_BASE + EL0_CODE_OFF));
	CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, PSTATE_EL0t));

	printf("VBAR_EL1      = 0x%lx\n", GUEST_BASE + VBAR_OFF);
	printf("EL1 stub      = 0x%lx  (hvc #0; eret)\n", GUEST_BASE + VEC_LOWER64);
	printf("EL0 code      = 0x%lx\n", GUEST_BASE + EL0_CODE_OFF);
	printf("entry (EL1)   = 0x%lx  (eret -> EL0)\n\n", GUEST_BASE + EL1_BOOT_OFF);

	/* ------------------------------------------------------------ the loop */
	int traps = 0;
	bool saw_first = false, passed = false;

	for (int iter = 0; iter < 16; iter++) {
		CHECK(hv_vcpu_run(vcpu));

		if (vexit->reason != HV_EXIT_REASON_EXCEPTION) {
			printf("exit reason %u (not an exception); continuing\n",
			    vexit->reason);
			continue;
		}

		uint64_t esr = vexit->exception.syndrome;
		uint32_t ec  = EC(esr);
		uint64_t pc  = 0;
		CHECK(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));

		if (ec != EC_HVC64) {
			printf("trap %d: EC=0x%02x %s  ESR=0x%016" PRIx64
			    "  PC=0x%" PRIx64 "\n", traps, ec, ec_name(ec), esr, pc);
			printf("        VA=0x%llx  IPA=0x%llx\n",
			    vexit->exception.virtual_address,
			    vexit->exception.physical_address);
			printf("\nFAIL: expected HVC64 (0x16).\n");
			break;
		}

		uint64_t x0 = 0, x8 = 0;
		CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
		CHECK(hv_vcpu_get_reg(vcpu, HV_REG_X8, &x8));

		uint64_t elr = 0, spsr = 0;
		CHECK(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1,  &elr));
		CHECK(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &spsr));

		traps++;
		printf("trap %d: EC=0x%02x %s\n", traps, ec, ec_name(ec));
		printf("        x8=%" PRIu64 " (syscall nr)  x0=0x%" PRIx64 "\n", x8, x0);
		printf("        PC=0x%" PRIx64 "  ELR_EL1=0x%" PRIx64
		    "  SPSR_EL1=0x%" PRIx64 "\n", pc, elr, spsr);

		/*
		 * ELR_EL1 must already point PAST the svc: on x86 the host
		 * advances RIP by 2 itself (src/main.c:255), and doing the
		 * equivalent here would silently skip an instruction.
		 */
		uint64_t expect_elr = GUEST_BASE + EL0_CODE_OFF + (saw_first ? 16 : 8);
		printf("        ELR %s (expected 0x%" PRIx64 ", i.e. past the svc)\n",
		    elr == expect_elr ? "OK" : "WRONG", expect_elr);

		if (!saw_first) {
			if (x8 != SYS_FIRST) {
				printf("\nFAIL: expected x8=%d\n", SYS_FIRST);
				break;
			}
			CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, SENTINEL));
			printf("        -> wrote x0=0x%llx, resuming\n\n", SENTINEL);
			saw_first = true;
		} else {
			if (x8 != SYS_SECOND) {
				printf("\nFAIL: expected x8=%d\n", SYS_SECOND);
				break;
			}
			if (x0 != SENTINEL) {
				printf("\nFAIL: x0 was 0x%" PRIx64 ", expected 0x%llx"
				    " - the value written by the host did not\n"
				    "      survive the return to EL0.\n", x0, SENTINEL);
				break;
			}
			printf("        -> x0 still holds the host's value\n");
			passed = true;
			break;
		}

		/*
		 * Resume. If HVF leaves PC on the hvc rather than past it we
		 * would spin here forever, so step over it explicitly.
		 */
		uint64_t resume = 0;
		CHECK(hv_vcpu_get_reg(vcpu, HV_REG_PC, &resume));
		if (resume == GUEST_BASE + VEC_LOWER64) {
			printf("        (PC still on the hvc; advancing by 4)\n");
			CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, resume + 4));
		}
	}

	printf("\n%s\n", passed
	    ? "PASS: svc at EL0 -> EL1 stub -> hvc -> host -> eret -> EL0.\n"
	      "      The EL1 trampoline works. PORTING-arm64.md section 2 holds."
	    : "FAIL: the EL1 trampoline design does not work as specified.");

	/*
	 * Cost. PORTING-arm64.md section 2 asserts the two exception-level
	 * transitions are "almost certainly still far cheaper than the syscall
	 * body itself" without measuring it. The harness is already here, so
	 * measure rather than assume.
	 *
	 * This is the floor: an empty round trip with no syscall dispatch on the
	 * host side at all.
	 */
	if (passed) {
		enum { BENCH_ITERS = 200000 };

		/* EL0: svc in a tight loop. b -8 == 0x17FFFFFE. */
		put32(g, EL0_CODE_OFF +  0, MOVZ_X8(SYS_FIRST));
		put32(g, EL0_CODE_OFF +  4, INSN_SVC0);
		put32(g, EL0_CODE_OFF +  8, 0x17FFFFFEu);

		CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC,   GUEST_BASE + EL1_BOOT_OFF));
		CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, PSTATE_EL1h));
		CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1,
		    GUEST_BASE + EL0_CODE_OFF));
		CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, PSTATE_EL0t));

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);

		int done = 0;
		while (done < BENCH_ITERS) {
			CHECK(hv_vcpu_run(vcpu));
			if (vexit->reason != HV_EXIT_REASON_EXCEPTION ||
			    EC(vexit->exception.syndrome) != EC_HVC64) {
				printf("bench: unexpected exit, aborting\n");
				break;
			}
			done++;
		}

		clock_gettime(CLOCK_MONOTONIC, &t1);

		double ns = (t1.tv_sec - t0.tv_sec) * 1e9 +
		            (t1.tv_nsec - t0.tv_nsec);
		printf("\n== cost ==\n");
		printf("%d empty round trips in %.1f ms\n", done, ns / 1e6);
		printf("%.0f ns per svc (EL0 -> EL1 -> host -> EL0), no dispatch\n",
		    ns / done);
	}

	hv_vcpu_destroy(vcpu);
	hv_vm_destroy();
	munmap(mem, GUEST_SIZE);
	return passed ? 0 : 1;
}
