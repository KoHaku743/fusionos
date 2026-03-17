# FusionOS

[![FusionOS CI](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml/badge.svg)](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml)

A minimal custom operating system with universal app compatibility.
Boots natively in QEMU (x86_64) and targets Apple Silicon (M2).

## Features

- Custom PID 1 init with orphan reaping
- `fsh` — FusionOS shell with built-in compat layer routing
- `fusion-run` — universal launcher (ELF/PE/Mach-O detection → Wine/Darling/Box64/FEX)
- `fusion-pkg` — package manager with APT and source package support
- `fusion-monitor` — ANSI TUI system monitor (CPU, memory, top processes, compat layer counts)
- Linux 6.6 LTS kernel with binfmt_misc, virtio, NTFS3, HFS+, namespaces, seccomp
- GRUB2 ISO with normal / verbose / recovery boot entries

## Quick Start

```bash
# Build everything (requires Ubuntu 22.04 and internet access)
bash build.sh

# Run in QEMU
bash run.sh
```

## Compat Layers

| Binary Format | Architecture | Launcher |
|---|---|---|
| ELF | x86 / x86-64 | kernel direct |
| ELF | ARM (ARMv7) | Box64 |
| ELF | AArch64 | FEX |
| PE32 / PE32+ | x86 / x86-64 | Wine / Wine64 |
| Mach-O / FAT | any | Darling |

## Project Layout

```
src/
  init/         — PID 1 init + initramfs init
  shell/        — fsh shell
  compat/       — detect library, fusion-run launcher, detect_test
  pkg/          — fusion-pkg package manager
  monitor/      — fusion-monitor TUI
kernel/         — Linux 6.6 kernel config
iso/            — GRUB2 boot config
scripts/        — binfmt_misc setup script
tests/          — integration test suite
build.sh        — full build pipeline
run.sh          — QEMU launcher
```

## Running Tests

```bash
bash tests/run_all.sh
```
