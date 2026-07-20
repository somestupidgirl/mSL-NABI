# NABI Hacking Guide

## Build from Source

```console
$ make
$ sudo make install
```

Artifacts land in `out/`. Never build as root: `make install` only *copies* what
is already in `out/`, so every build artifact stays owned by you and `make clean`
never needs `sudo`.

Override the install prefix with `PREFIX` (default `/usr/local`):

```console
$ make PREFIX=/opt/local && sudo make PREFIX=/opt/local install
```

This installs the `nabi` command in your system. The `nabi` command is a simple perl script that checks if a Linux system is ready on your system and install it otherwise before executing the "real" nabi binary.

### Architecture

Only `x86_64` builds today — the VMM backend is VT-x, so it needs an Intel Mac
with `kern.hv_support`. On Apple Silicon `make` stops with a pointer to
[PORTING-arm64.md](PORTING-arm64.md); `make ARCH=x86_64` cross-builds the Intel
backend, which is useful for checking that a change still compiles even though
the result cannot run there.

There is deliberately no `ARCH=universal`. The x86_64 build runs x86-64 Linux
guests and an arm64 build will run aarch64 guests: those are two programs, not
two slices of one, and a fat binary would present a guest ABI depending on which
slice the kernel picked.

### Code signing

The binary is signed at the end of every link, with
`installer/nabi.entitlements` granting `com.apple.security.hypervisor`. On Intel
this is harmless; on Apple Silicon `hv_vm_create()` returns `HV_DENIED` without
it, so it is mandatory there.

Ad-hoc signing (the default, `SIGNCERT=-`) is enough for a development build.
Set `SIGNCERT` to a keychain identity to sign with a real certificate;
distribution needs a Developer ID with the entitlement granted by Apple.

Nothing may modify the binary after the signature is applied — a later `strip`,
`lipo` or `install_name_tool` invalidates it and silently takes the entitlement
with it.

### Tests

```console
$ make check
```

Runs `test/test.rb` against the prebuilt Linux guest binaries committed under
`test/*/build/`. It needs a host that can actually run those guests (an x86_64
build on an Intel Mac with VT-x) and *skips* with an explanation otherwise,
rather than reporting a failure that says nothing about the code.

Note that `test/test.mk`, which **rebuilds** those guest binaries, shells out to
a Linux box at `idylls.jp` that has not existed for years. Running the tests
does not need it; regenerating them does.

The "real" nabi binary, which is made from `src/main.c`, requires some mandatory command line options. If you want to execute it directly, you will need to start the binary with the following manner:

```console
$ INSTALL_PREFIX/libexec/nabi -m ROOT_PATH PATH_TO_INIT
```
where `ROOT_PATH` is a path to the directory that is treated as the root mount point in the Linux box, and `PATH_TO_INIT` is a path to the first command to be run in the boot sequence, like `/bin/bash`.

`noahstrap` helps you set up a Linux environment on your local machine.
It retrieves a ready-to-use distro image from the Internet and extracts it to a specified directory.
`noahstrap` is installed via homebrew. It keeps its upstream name and tap — it
lives outside this repository and was not renamed with the rest of the fork.

```console
$ brew install linux-noah/noah/noahstrap
$ noahstrap --help  # prints help message
```

Note that `noahstrap` fetches an **x86-64** tree. The arm64 port will need
either an `--arch` flag added upstream or a `debootstrap --arch=arm64` tarball;
see [INTEGRATION.md](INTEGRATION.md).

## Debugging

There are several methods to debug `nabi`.

The first option is to use lldb, of course. Since the nabi command indirectly invokes the nabi binary through a perl script, it would be convenient to modify the script to start up the binary with lldb.

The second option is to use _meta-strace_. Because we still don't have implemented the ptrace system call, we cannot use strace to trace the log inside the Linux box. To tackle this problem, we provide a feature called meta-strace, that logs system call entering and retirement at the `meta level`, which means outside virtual machines. This feature is enabled via `--strace OUTFILE` option for the `nabi` command. When you add a new system call and check the behavior, it might be helpful to add new strace {pre,post} hooks in the `src/meta_strace.c`. This should greatly improve the appearance of the trace logs.

The third option is to use the _output_ option. This is mere debug logs that are emit using the `printk` function in nabi's source codes. This feature is enabled with `--output OUTFILE` option.

## Source Structure

Sources are placed in the following rules:

- `bin/`: the `nabi` perl script inhabits
- `lib/`: sources for the VMM components
- `src/foo/*`: Linux subsystem emulations put in the corresponding directories
- `src/meta_strace.c`: meta-strace, see Debugging section
- `src/base.c`: general virtual kernel primitives, like copy_from_user
- `src/main.c`: the entry point
