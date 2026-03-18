# FusionOS

[![FusionOS CI](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml/badge.svg)](https://github.com/KoHaku743/fusionos/actions/workflows/build.yml)

A minimal, bootable operating system with universal app compatibility —
run Windows (PE), macOS (Mach-O), and Linux (ELF) binaries transparently.
Targets x86-64 QEMU initially; ARM64 / Apple Silicon next.

## Quick Start

### Build everything (requires Ubuntu 22.04 or compatible)

```bash
# Install dependencies
sudo apt-get install -y gcc make wget xz-utils bzip2 cpio gzip bc flex bison \
    libssl-dev libelf-dev grub-pc-bin grub-efi-amd64-bin grub-common \
    xorriso mtools binutils

# Build the ISO (downloads kernel + BusyBox automatically)
./build.sh all

# Launch in QEMU
./run.sh
```

### Build only the userspace tools (no kernel download)

```bash
./build.sh rootfs
```

### Run tests

```bash
tests/run_all.sh
```

## Project Layout

```
├── build.sh                  Full ISO build pipeline
├── run.sh                    QEMU launcher
├── iso/boot/grub/grub.cfg   GRUB2 boot menu
├── kernel/fusionos.config   Linux 6.6 LTS kernel config
├── scripts/setup_binfmt.sh  binfmt_misc registration
├── src/
│   ├── init/
│   │   ├── initramfs_init.c PID 1 in initramfs (switch_root)
│   │   └── init.c           PID 1 on the real rootfs
│   ├── shell/shell.c        fsh — FusionOS shell
│   ├── compat/
│   │   ├── detect.h/.c      Binary format detection library
│   │   ├── detect_test.c    Standalone detection test binary
│   │   └── fusion_run.c     fusion-run universal launcher
│   ├── pkg/fusion_pkg.c     fusion-pkg package manager
│   └── monitor/monitor.c    fusion-monitor TUI system monitor
└── tests/
    ├── run_all.sh            Test runner
    ├── test_detect.sh        Binary detection tests
    ├── test_shell.sh         fsh shell tests
    └── test_build.sh         Build artefact verification
```

## Compat Layers

| Layer   | Handles                        | Backend          |
|---------|--------------------------------|------------------|
| Wine    | Windows PE32 / PE64 (x86/x64)  | `wine` / `wine64`|
| Darling | macOS Mach-O (x86-64, ARM64)   | `darling`        |
| Box64   | ARM64 → x86-64 translation     | `box64`          |
| FEX     | ARM64 → x86 / x86-64          | `FEXLoader`      |
| Native  | x86-64 ELF                     | kernel           |

## Package Manager

```bash
fusion-pkg install wine      # install Wine via apt
fusion-pkg install box64     # install Box64 via apt
fusion-pkg search compat     # search the registry
fusion-pkg list              # list installed packages
fusion-pkg info wine         # detailed package information
fusion-pkg update            # update all installed packages
```

## License

MIT
