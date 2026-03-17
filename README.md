# FusionOS

[![FusionOS CI](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml/badge.svg)](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml)

A minimal operating system with universal application compatibility — runs ELF (x86/ARM/RISC-V), PE (Windows), and Mach-O (macOS) binaries natively via integrated compatibility layers.

## Project Structure

- `src/init/` — PID 1 init and initramfs init
- `src/shell/` — `fsh`: FusionOS shell with built-in compat routing
- `src/compat/` — binary format detection library, `fusion-run` universal launcher
- `src/pkg/` — `fusion-pkg` package manager
- `src/monitor/` — `fusion-monitor` ANSI TUI system monitor
- `kernel/` — Linux 6.6 LTS kernel config
- `iso/boot/grub/` — GRUB2 bootloader config
- `scripts/` — binfmt_misc setup scripts
- `tests/` — integration test suite

## Quick Start

```bash
# Build all tools
bash tests/test_build.sh

# Run all tests
bash tests/run_all.sh

# Build full ISO (requires kernel build toolchain)
bash build.sh

# Launch in QEMU
bash run.sh
```

## Compatibility Layers

| Binary Format | Arch | Launcher |
|---|---|---|
| ELF | x86 / x86-64 | kernel native |
| ELF | ARM / ARM64 | Box64 / FEX |
| PE (Windows) | x86 / x86-64 | Wine |
| Mach-O (macOS) | x86 / ARM64 | Darling |
