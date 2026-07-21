# arm64 end-to-end smoke test

The smallest real aarch64 Linux binaries, run under a natively-built `nabi` to
exercise the whole pipeline at once: ELF load, stage-1/stage-2 translation, the
EL1 trampoline, syscall dispatch through the generated aarch64 table, and the
guest-code cache sync.

- `exit42` — `exit(42)`; checks exit-code propagation.
- `hello`  — `write(1, "hello arm64!\n", 14); exit(0)`; checks a syscall with
  guest memory arguments and stdout.

The ELF binaries are committed prebuilt (as the x86 guest tests under
`test/*/build/` are), so the check needs no cross-toolchain. The `.s` sources
are here for reference; rebuild with an aarch64-linux clang + ld.lld:

    clang -target aarch64-linux-gnu -c exit42.s -o exit42.o
    ld.lld -static -e _start exit42.o -o exit42

Run via `make check-smoke` (Apple Silicon only; needs a natively-built nabi).
