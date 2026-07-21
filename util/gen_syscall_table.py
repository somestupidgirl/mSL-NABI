#!/usr/bin/env python3
#
# Generate the aarch64 syscall table (include/syscall_arm64.h) from the kernel's
# asm-generic/unistd.h.
#
# aarch64 uses the *generic* syscall numbering, which is unrelated to the x86-64
# numbering NABI was built against - read is 63 not 0, write 64 not 1, mmap 222
# not 9. Hand-transcribing that mapping is how this port would quietly die, so
# it is generated instead: reproducible from a named kernel header, and every
# wired-up and every missing syscall is reported for audit.
#
# Usage:
#   util/gen_syscall_table.py <asm-generic/unistd.h> [x86-syscall.h] > out.h
#
# The second argument (default include/syscall.h) is the existing x86 table,
# read only to learn which handler *names* NABI actually implements: a name is
# wired into the aarch64 table iff a _sys_<name> handler exists for it, which is
# exactly the set of non-"unimplemented" entries there.
#
# A report goes to stderr: aarch64 syscalls with no NABI handler (become
# "unimplemented"), and NABI handlers with no aarch64 number (x86 legacy, e.g.
# open/stat/fork - dropped, since aarch64 glibc never calls them).

import re
import sys


def implemented_names(x86_syscall_h):
    text = open(x86_syscall_h).read()
    names = set()
    for m in re.finditer(r'SYSCALL\(\s*\d+\s*,\s*(\w+)\s*\)', text):
        if m.group(1) != 'unimplemented':
            names.add(m.group(1))
    return names


def parse_generic_unistd(path):
    """Return {name: number} for every aarch64 syscall in the header.

    Two forms matter: plain `#define __NR_<name> <int>`, and the 32/64 split
    `#define __NR_<name> __NR3264_<name>` where `__NR3264_<name>` is defined to
    an int elsewhere. The latter has to be resolved one level.
    """
    text = open(path).read()

    nr3264 = {}
    for m in re.finditer(r'#define\s+__NR3264_(\w+)\s+(\d+)', text):
        nr3264[m.group(1)] = int(m.group(2))

    table = {}
    for m in re.finditer(r'#define\s+__NR_(\w+)\s+(.+)', text):
        name, val = m.group(1), m.group(2).strip()
        # Not real syscalls: the total count and the arch-specific base marker.
        if name in ('syscalls', 'arch_specific_syscall'):
            continue
        if val.isdigit():
            table[name] = int(val)
        elif val.startswith('__NR3264_'):
            key = val[len('__NR3264_'):]
            if key in nr3264:
                table[name] = nr3264[key]
    return table


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    unistd = sys.argv[1]
    x86_h = sys.argv[2] if len(sys.argv) > 2 else 'include/syscall.h'

    impl = implemented_names(x86_h)
    aa = parse_generic_unistd(unistd)

    by_nr = {}
    for name, nr in aa.items():
        by_nr.setdefault(nr, name)
    max_nr = max(by_nr)
    count = max_nr + 1

    wired = sorted(n for n in aa if n in impl)
    missing = sorted(n for n in aa if n not in impl)
    dropped = sorted(impl - set(aa))

    # ---- report (stderr) ----
    r = sys.stderr
    print(f"aarch64 syscalls in header: {len(aa)}  (max nr {max_nr})", file=r)
    print(f"wired to a NABI handler:    {len(wired)}", file=r)
    print(f"no NABI handler (-> unimplemented): {len(missing)}", file=r)
    print(f"NABI handlers with no aarch64 number (dropped legacy): "
          f"{len(dropped)}", file=r)
    print(f"\nunimplemented aarch64 syscalls:\n  {' '.join(missing)}", file=r)
    print(f"\ndropped x86 legacy handlers:\n  {' '.join(dropped)}", file=r)

    # ---- table (stdout) ----
    #
    # The dropped x86-legacy handlers (open, stat, fork, ... - impl with no
    # aarch64 number) still have _sys_<name> definitions and meta_strace
    # wrappers in the C sources, and those wrappers reference LSYS_<name>, which
    # only exists for names in this table. So they are appended past the real
    # range as a compat tail: it gives them an LSYS_ id and a prototype without
    # disturbing the real numbering. Real aarch64 glibc never issues those
    # numbers; a guest that deliberately did would reach the legacy handler
    # instead of the unknown-syscall path, which is benign (the handlers are
    # safe, and most return -ENOSYS).
    compat = dropped
    total = count + len(compat)

    src = unistd.replace('\\', '/').split('/')[-1]
    o = sys.stdout
    print("/*", file=o)
    print(" * aarch64 syscall table - GENERATED, do not edit by hand.", file=o)
    print(f" * Regenerate: util/gen_syscall_table.py <{src}>", file=o)
    print(" * See PORTING-arm64.md section 3.2.", file=o)
    print(" */", file=o)
    print("#include <stdint.h>", file=o)
    print("", file=o)
    print("#define SYSCALLS \\", file=o)
    for n in range(count):
        name = by_nr.get(n)
        sym = name if (name and name in impl) else 'unimplemented'
        print(f"  SYSCALL({n}, {sym}) \\", file=o)
    if compat:
        print("  /* x86-legacy handlers with no aarch64 number - see the "
              "generator. */ \\", file=o)
        for i, name in enumerate(compat):
            print(f"  SYSCALL({count + i}, {name}) \\", file=o)
    print("", file=o)
    print(f"#define NR_SYSCALLS {total}", file=o)


if __name__ == '__main__':
    main()
