# NABI: build system and mSL integration

**Companion to [PORTING-arm64.md](PORTING-arm64.md).** That document covers the
aarch64 guest port. This one covers the two things that have to happen around it:
replacing CMake with a plain Makefile in the style of mSL/FHS and mSL/ProcFS, and
wiring NABI into the rest of the mSL tree.

**Status:** plan only. No code written yet.

**Decisions taken:** both architectures stay buildable (`ARCH` selects the VMM
backend); the binary is renamed `noah` → `nabi`.

---

## 1. Why the build system comes first

The arm64 port cannot start until the build system changes, whatever we think of
CMake on the merits.

Hypervisor.framework on Apple Silicon requires the `com.apple.security.hypervisor`
entitlement **and a valid code signature**. On Intel an unsigned binary could call
`hv_vm_create`; on arm64 it cannot. [CMakeLists.txt](CMakeLists.txt) has no
`codesign` step at all, because it never needed one. So Phase 0 of the port plan —
the EL1 trampoline spike that gates the entire design — cannot even be launched by
today's build.

That makes the ordering unambiguous: convert the build, add signing, *then* spike.
The alternative (a throwaway shell script to build the spike) means writing the
signing logic twice and validating the design against a build that isn't the real
one.

There is a second, smaller reason. Phase 1 of the port plan calls for splitting the
VMM behind an arch-neutral interface, `lib/vmm_x86.c` vs `lib/vmm_arm64.c`. Source
selection on `ARCH` *is* the build half of that abstraction. Doing the conversion
first means Phase 1 only has to write the C.

---

## 2. What CMake is actually carrying

Four things, all cheap to replace. This is a small build, and the conversion is
mechanical.

| CMake feature | Makefile replacement |
|---|---|
| `project(noah VERSION 0.5.1)` | `VERSION` file + `$(shell cat VERSION)`, matching FHS and ProcFS |
| `configure_file(include/version.h.in)` | drop the header; pass `-DNABI_VERSION=\"$(VERSION)\"` in `CFLAGS` as FHS does |
| `configure_file(bin/noah.in)` | one `sed` rule substituting `@PROJECT_VERSION@` and `@CMAKE_INSTALL_PREFIX@` |
| `enable_testing()` + [test/CMakeLists.txt](test/CMakeLists.txt) | a `check:` target invoking [test/test.rb](test/test.rb) |

Two things get deleted rather than ported:

- The `MACOS_PRE_16` guard ([CMakeLists.txt:7](CMakeLists.txt#L7)) — dead on Apple
  Silicon, and the condition it guards is about macOS Sierra.
- [Jenkinsfile](Jenkinsfile) — it runs `cmake -DCMAKE_BUILD_TYPE=Release .. && make`
  against an unmaintained upstream Jenkins with a Slack webhook. It will not survive
  the conversion and is not worth porting; if CI is wanted, it should be a GitHub
  Actions workflow on a macOS runner, and it can only build (no runner grants
  `kern.hv_support`, so `check` cannot run in CI).

[HACKING.md](HACKING.md) documents the `ccmake` flow and needs rewriting alongside.

---

## 3. Makefile design

Conventions lifted directly from [mSL-ProcFS/Makefile](../mSL-ProcFS/Makefile) and
[mSL-FHS/Makefile](../mSL-FHS/Makefile), because consistency across the four repos
is worth more than local optimisation:

- `OUT := out`, everything lands there, `clean` is `rm -rf $(OUT)`.
- **Never build as root.** `install` only *copies* already-built artifacts, so every
  build product stays owned by the invoking user and `clean` never needs sudo.
- `VERSION` file is the single source of truth.
- `require-root` / `require-built` guard targets.
- `SIGNCERT ?= -` with keychain validation, lifted verbatim from
  [mSL-ProcFS/Makefile.inc](../mSL-ProcFS/Makefile.inc) — including the fallback
  that warns and drops to ad-hoc when a stale `SIGNCERT` is exported in the
  environment. Ad-hoc signing carries the hypervisor entitlement fine for
  development builds.

### 3.1 Arch dispatch

```make
NATIVE_ARCH := $(shell uname -m)
ARCH ?= $(NATIVE_ARCH)

ifeq ($(ARCH),arm64)
    ARCH_SRCS := lib/vmm_arm64.c src/arch/arm64/mmu.c src/arch/arm64/vector.S
    ARCHFLAGS := -arch arm64
else ifeq ($(ARCH),x86_64)
    ARCH_SRCS := lib/vmm_x86.c src/arch/x86/mmu.c
    ARCHFLAGS := -arch x86_64
else
    $(error Unknown ARCH=$(ARCH). Use arm64 or x86_64)
endif
```

Note the deliberate absence of `ARCH=universal`, which both FHS and ProcFS support.
A fat NABI binary is meaningless: the x86 build runs x86-64 Linux guests via VT-x
and the arm64 build runs aarch64 guests via the ARM HVF API. They are not two builds
of one program, they are two programs. A `lipo`'d binary would present a guest ABI
that depends on which slice the kernel picked. Do not add it.

`arm64e` is likewise not a target — third-party arm64e userland binaries are not
supported for distribution and the ABI is explicitly unstable. Plain `arm64` is
correct here. (ProcFS defaults to `arm64e` because kexts *require* that ABI; NABI is
ordinary userland and must not copy it.)

### 3.2 The link and sign step

```make
$(OUT)/nabi: $(COMMON_SRCS) $(ARCH_SRCS) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $^ -framework Hypervisor -lpthread
	codesign --force --sign $(SIGNCERT) \
	         --entitlements installer/nabi.entitlements $@
```

with `installer/nabi.entitlements` declaring `com.apple.security.hypervisor`.

The signature must be applied **after** every write to the binary. Anything that
modifies it afterwards (a `strip`, a `lipo`, an `install_name_tool`) invalidates the
signature and the entitlement goes with it — the failure surfaces much later as
`hv_vm_create` returning `HV_DENIED`, which reads like a permissions problem rather
than a build problem. Worth a comment in the Makefile at that line.

### 3.3 Divergence: the default target

FHS and ProcFS both default to building a `.dmg` (`all: build dmg`,
`all: clean kextfs tools plists gui pkg dmg`). NABI should not.

Those projects ship kexts and filesystem bundles; iterating on them means an install
and usually a reboot, so packaging on every build is nearly free relative to the
cycle time. NABI is a single userland binary with a fast, tight debug loop. Making
every `make` produce a disk image would add tens of seconds to a cycle measured in
seconds.

```make
all: build
```

with `pkg`, `dmg` and `distcheck` as explicit targets. `distcheck` should keep the
same shape as the other two (clean build, then verify the payload actually contains
every component via `lsbom`) — that check has caught real payload regressions.

### 3.4 The rename

`noah` → `nabi`, matching the repo name and the `fhsctl` / `fhsxd` / `procfsd`
naming across mSL. Affected:

- `bin/noah.in` → `bin/nabi.in`; the perl wrapper's `$noahdir = "$ENV{HOME}/.noah"`
  and its `noahstrap` invocation (see §4.2).
- `man/noah.1` → `man/nabi.1`, plus `man/noah.md` / `man/noah.js`.
- `include/version.h.in` macros `NOAH_VERSION` etc. — removed entirely per §2.
- `INTERP_PATH` in mSL-imgact (§4.1).

FHS handles exactly this situation with a `migrate:` target that runs before
`install` — it stops the old daemon, moves `/var/db/msl.*` state to `/var/db/fhs.*`,
and removes the superseded binaries and bundles. NABI's migration is much smaller
(no daemon, no persisted state): remove `$(PREFIX)/libexec/noah` and
`$(PREFIX)/bin/noah`, and leave `~/.noah/tree` alone — it may be a multi-gigabyte
rootfs the user cannot cheaply re-download, so it should be *adopted*, not deleted.
See §4.2.

One caveat worth stating plainly: the old `noah` binary may have been made setuid
root by the perl wrapper ([bin/noah.in:44-49](bin/noah.in#L44)). The migrate target
must remove that binary rather than leave an orphaned setuid-root executable behind
on the user's disk.

---

## 4. mSL integration

The four repos already share surface conventions: `com.beako.*` identifiers, the
`/Applications/mSL` folder, a `VERSION` file, `out/`, and the `pkg` → `dmg` →
`distcheck` chain. Those are worth adopting in NABI for consistency, but they are
not the interesting part. Three couplings are substantive — one of which turns out
to be a coupling that has to be *cut*.

### 4.1 The exec path — mSL-imgact is not viable on Apple Silicon

[mSL-imgact](../mSL-imgact) was built to hook XNU's `execsw` "Interpreter Script"
image activator ([imgact_linux.c:79-87](../mSL-imgact/imgact_linux.c#L79)) and hand
Linux ELF binaries to an interpreter named by a sysctl. Its compiled-in default
already points at noah (`INTERP_PATH`,
[imgact_linux.h:8](../mSL-imgact/imgact_linux.h#L8)), so the intent was transparent
exec: `./some-linux-binary` from a native shell just works.

**That technique cannot work on Apple Silicon.** Three independent blockers, any one
of which is fatal, verified on macOS 26 / M5 (Mac17,3):

1. **`_execsw` does not exist in the arm64e kernel.** The arm64e kernels
   (`/System/Library/Kernels/kernel.release.t*`) export 6894 symbols — the KPI
   surface — and `_execsw` is not among them. There is nothing for a proxy kext to
   harvest. Note that [Makefile:104](../mSL-imgact/Makefile#L104) harvests from
   `/System/Library/Kernels/kernel`, which is a **Mach-O x86_64** leftover; it does
   contain `_execsw`, at the x86-shaped address `0xffffff8000f02790`. That is an
   Intel address being re-exported into an `-arch arm64e` kext.
2. **The table is `const`.** XNU declares `execsw[]` as `const`, with
   `int (*const ex_imgact)(...)`, so it lands in `__DATA_CONST` — read-only after
   boot.
3. **SPTM.** The kernel carries a `__DATA_SPTM` segment; page-table integrity is
   enforced by the Secure Page Table Monitor, below the kernel. Patching kernel
   const data is not a SIP question — there is no privilege level available to a
   kext that can do it.

The in-tree attempt to sidestep (1) by declaring a local `struct kexecsw` in
[kexec.c:408-414](../mSL-imgact/lib/libkexec/kexec.c#L408) does not help, because
symbol resolution was never the real obstacle: `exec_activate_image()` iterates the
*kernel's* table, so a kext-local copy is never consulted by anything. (It also has
three mechanical faults — the variable is named `execsw` while
[imgact_linux.c:24](../mSL-imgact/imgact_linux.c#L24) declares `extern kexecsw[]`,
the array is `const` so the assignments at
[imgact_linux.c:82](../mSL-imgact/imgact_linux.c#L82) will not compile against it,
and the kext was never loadable to begin with.)

**Endpoint Security is not a substitute.** `ES_EVENT_TYPE_AUTH_EXEC` can *deny* an
exec but cannot *substitute* one. Denying and separately relaunching under NABI
breaks pid, `wait()`, stdio inheritance, job control and exit status — the parent
sees a failed exec, not a running program. It also requires an Apple-granted ES
entitlement, which is a distribution blocker for an open-source project.

#### What this actually costs: almost nothing

NABI does not need an image activator. **Inside the guest, NABI is the kernel** — it
implements `execve` itself in [src/proc/exec.c](src/proc/exec.c), so a Linux process
spawning another Linux process never reaches XNU's activators. imgact only ever
bought launching a Linux binary from a *native* macOS shell: ergonomics, not
capability.

It is also ergonomics WSL does not have. Windows does not exec ELF binaries from
`cmd.exe`; interop goes through `wsl.exe <cmd>`. The subsystem is entered, not
dissolved into the host.

So this **removes** a dependency rather than deferring one, and it removes the
awkward bootstrapping problem where imgact could not be tested until the arm64 port
worked.

#### Replacement: generated wrapper scripts on PATH

Ship a directory of shims — `/usr/local/lib/nabi/bin/<tool>`, each one:

```sh
#!/bin/sh
exec /usr/local/libexec/nabi -- /usr/bin/<tool> "$@"
```

generated from the guest rootfs at install time and added to PATH. This is
essentially what WSL interop does, it is entirely userspace, and it degrades
honestly: `ls` in a native shell is still macOS `ls`, and nobody is misled about
which one ran. Explicit `nabi ./foo` remains the primitive underneath.

**Recommendation for the imgact repo:** park it rather than push it. There is little
to salvage — NABI has its own ELF loader, so `elf_check_header` and the
`include/sys/elf*.h` copies are redundant, and `kexec.c`'s shebang parsing is only
meaningful inside the kernel. If kept, keep it as an Intel-only historical branch
with a README recording why the approach cannot work on Apple Silicon; that is
genuinely useful to the next person who has the idea.

### 4.2 mSL-FHS should own the rootfs

Today the rootfs story is three-way inconsistent: the perl wrapper uses
`~/.noah/tree` ([bin/noah.in:32](bin/noah.in#L32)), imgact assumes `/compat/linux`,
and provisioning goes through `noahstrap -p ubuntu` — a Homebrew formula living
outside this repo that fetches an **x86-64** tree. The arm64 port needs an arm64
tree regardless, so this has to be touched anyway; either patch `noahstrap` for an
`--arch` flag or point `bin/nabi.in` at a `debootstrap --arch=arm64` tarball.

The proposal: make the guest rootfs an FHS-managed component, alongside `/home`,
`/mnt`, `/boot`, `/media`. FHS already knows how to create paths on the read-only
system volume via `/etc/synthetic.conf`, has `enable` / `disable` / `check`
semantics per component, and surfaces them in a prefPane.

The real payoff is namespace agreement, not tidiness. FHS makes `/home/$USER` a
genuine path on the host. If NABI's guest maps that same path through instead of
giving the guest a private copy, a file written by a Linux process and a file
written by a native macOS process are the same file. That is the difference between
an emulator and a subsystem.

Related: [util/setup.sh](util/setup.sh) currently generates the guest's
`/etc/passwd` and `/etc/group` from the host with
[util/mkpasswd.pl](util/mkpasswd.pl) and [util/mkgroup.pl](util/mkgroup.pl). If FHS
owns `/home`, it should own this too — the uid/gid mapping and the home-directory
layout are one decision, and splitting them across two repos guarantees they drift.

Migration for existing users: if `~/.noah/tree` exists, adopt it as the rootfs
rather than re-provisioning. It may be several gigabytes.

### 4.3 mSL-ProcFS can back the guest's /proc

NABI synthesizes `/proc` inside the guest from `src/fs/fs.c`. ProcFS implements a
real `/proc` on the host, and already has an explicitly Linux-flavoured mode — it
persists `/var/db/procfs.linux` and `/var/db/procfs.linux_version`
([mSL-ProcFS/Makefile](../mSL-ProcFS/Makefile)).

If ProcFS is mounted, NABI's fs layer can pass those paths through instead of
maintaining a parallel implementation. Two implementations of `/proc` in one project
will disagree, and the disagreement will surface as a Linux tool silently reading
wrong data.

This is the least urgent of the three — it is an optimisation and a
de-duplication, not a capability. It should come after the port is running real
binaries, when it is clear which `/proc` nodes guests actually touch. But
`procfs.linux_version` should probably be driven by NABI (which knows what kernel
version it claims to emulate) rather than configured independently in two places.

### 4.4 Shared conventions to adopt

Low-effort, high-consistency, no design questions:

- `VERSION` file; `out/`; never-build-as-root; `install` copies only.
- `com.beako.*` identifiers.
- `/Applications/mSL` if NABI ever grows a GUI. It does not have one now and does
  not obviously need one — resist adding a menu-bar app for symmetry alone.
- `pkg` / `dmg` / `distcheck` with the `lsbom` payload verification.
- A top-level `mSL-XNU/Makefile` recursing into all four repos with shared
  `ARCH` / `SIGNCERT` / `VERSION` policy. Natural once NABI has a Makefile to
  recurse into — it is the only holdout.

---

## 5. Ordering

1. **Restore [PORTING-arm64.md](PORTING-arm64.md).** Done.
2. **CMake → Makefile, x86_64 only, no behaviour change.** Verifiable against a real
   baseline: [test/test.rb](test/test.rb) passes exactly as it does today. Includes
   the `noah` → `nabi` rename and the `migrate:` target.
3. **Entitlement + codesign in that Makefile.** Verifiable on arm64: a trivial
   binary that calls `hv_vm_create` and gets something other than `HV_DENIED`.
4. **Phase 0 spike** — the EL1 trampoline proof from the port plan, built by the new
   Makefile. Still gates everything downstream. If `svc` does not come back as
   `HV_EXIT_REASON_EXCEPTION` with `ESR_EL2.EC == 0x16`, §2 of the port plan is
   wrong and needs revisiting before any NABI code is touched.
5. **Phase 1 onward** per [PORTING-arm64.md](PORTING-arm64.md).

Integration work (§4) interleaves rather than following. §4.1 is now a *removal* —
no kext, no exec hook, no work in the critical path; the wrapper-script replacement
is packaging and can land any time after the port runs real binaries. §4.2 blocks
Phase 5 (dynamic linking needs a working arm64 rootfs), and is the only integration
item that gates the port. §4.3 comes last.

Nothing in §4 needs doing out of order. The earlier draft of this document flagged
"confirm mSL-imgact still loads" as the one thing worth checking early; that check
has since been done and the answer is that it cannot work at all (§4.1), so the
dependency is gone rather than pending.
