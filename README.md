# mSL/NABI

**NABI** (Noah ABI) is the Linux ABI layer of **mSL**, the macOS Subsystem for
Linux. It is implemented as a hypervisor that traps Linux system calls and
translates them into Darwin's, and it interprets ELF files so that Linux binary
executables run directly, without modification.

NABI is a fork of [Noah](https://github.com/linux-noah/noah), which is no longer
maintained. For the technical background, see the original
[academic paper](https://dl.acm.org/doi/abs/10.1145/3381052.3381327).

__NABI is experimental.__ Many Linux applications do not work, mostly due to
missing system calls.

<img src="images/screenshot.png" width="600">

## Status

**x86_64 only.** The VMM backend is VT-x, so NABI currently requires an Intel
Mac. The Apple Silicon port is planned but not started — see
[PORTING-arm64.md](PORTING-arm64.md) for the design and
[INTEGRATION.md](INTEGRATION.md) for how NABI fits into the rest of mSL.

## Quick Start

```console
$ make
$ sudo make install
$ nabi
```

On first run, `nabi` downloads and installs a Linux environment in your home
directory (by default Ubuntu, in `~/.nabi/tree`). A rootfs left by a previous
`noah` install at `~/.noah/tree` is adopted rather than re-downloaded.

The upstream Homebrew and MacPorts packages install the original `noah`, not
NABI, and are not updated for this fork.

## The mSL components

| | |
|---|---|
| [mSL/NABI](.) | Linux ABI — runs Linux binaries |
| mSL/FHS | Filesystem Hierarchy Standard layout on macOS |
| mSL/ProcFS | a real `/proc` |

## Hacking

See [HACKING.md](HACKING.md).

## LICENSE

Dual MITL/GPL, for all files without explicit notation.
