# FusionOS ISO Build Guide

FusionOS is a minimal x86-64 operating system built around Linux 6.6 LTS and
a custom userspace that transparently runs ELF, Windows PE, and macOS Mach-O
binaries through compatibility layers (Wine, Darling, Box64, FEX-Emu).

This document describes every step needed to build a bootable FusionOS ISO
image from source.

---

## Table of Contents

1. [Repository Layout](#1-repository-layout)
2. [Build Dependencies](#2-build-dependencies)
3. [Quick Start](#3-quick-start)
4. [Step-by-Step Build](#4-step-by-step-build)
   - 4.1 [Kernel](#41-kernel)
   - 4.2 [BusyBox](#42-busybox)
   - 4.3 [Userspace Components](#43-userspace-components)
   - 4.4 [Initramfs](#44-initramfs)
   - 4.5 [ISO Assembly](#45-iso-assembly)
5. [Component Reference](#5-component-reference)
   - 5.1 [Binary Detection Library (detect)](#51-binary-detection-library-detect)
   - 5.2 [Initramfs Init](#52-initramfs-init)
   - 5.3 [Main Init (PID 1)](#53-main-init-pid-1)
   - 5.4 [FusionOS Shell (fsh)](#54-fusionos-shell-fsh)
   - 5.5 [fusion-run](#55-fusion-run)
   - 5.6 [fusion-pkg](#56-fusion-pkg)
   - 5.7 [fusion-monitor](#57-fusion-monitor)
6. [Kernel Configuration](#6-kernel-configuration)
7. [GRUB2 Boot Menu](#7-grub2-boot-menu)
8. [ISO Directory Structure](#8-iso-directory-structure)
9. [Testing the ISO](#9-testing-the-iso)
10. [Customisation](#10-customisation)

---

## 1. Repository Layout

```
fusionos/
├── build.sh                  # Top-level build script
├── fusionos_iso_build.md     # This document
├── iso/
│   └── boot/
│       └── grub/
│           └── grub.cfg      # GRUB2 boot menu
├── kernel/
│   └── fusionos.config       # Linux 6.6 LTS kernel config
└── src/
    ├── compat/
    │   ├── detect.h          # Binary format detection API
    │   ├── detect.c          # ELF / PE / Mach-O detection implementation
    │   ├── detect_test.c     # CLI test tool for the detection library
    │   └── fusion_run.c      # Universal binary launcher
    ├── init/
    │   ├── initramfs_init.c  # Tiny initramfs /init (mounts filesystems,
    │   │                     #   finds root device, calls switch_root)
    │   └── init.c            # PID-1 init (mounts, spawns fsh, reaps children)
    ├── monitor/
    │   └── monitor.c         # System monitor (CPU, memory, processes)
    ├── pkg/
    │   └── fusion_pkg.c      # fusion-pkg package manager
    └── shell/
        └── shell.c           # fsh – FusionOS shell
```

---

## 2. Build Dependencies

### Mandatory

| Tool | Purpose |
|------|---------|
| `gcc` | C compiler for all userspace components |
| `make` | Kernel and BusyBox build system |
| `tar`, `xz`, `bzip2` | Archive extraction |
| `cpio`, `gzip` | Initramfs creation |
| `grub-mkrescue` | ISO assembly with GRUB2 embedded |
| `xorriso` | ISO writer backend (used by `grub-mkrescue`) |
| `wget` or `curl` | Source tarball download |

### Optional

| Tool | Purpose |
|------|---------|
| `qemu-system-x86_64` | ISO smoke-test (`build.sh test`) |
| `grub-pc-bin` | BIOS-mode GRUB modules |
| `grub-efi-amd64-bin` | UEFI-mode GRUB modules |

### Installing on Debian / Ubuntu

```bash
sudo apt-get install \
    gcc make tar xz-utils bzip2 cpio gzip \
    grub-pc-bin grub-efi-amd64-bin xorriso \
    wget curl \
    qemu-system-x86
```

### Installing on Fedora / RHEL

```bash
sudo dnf install \
    gcc make tar xz bzip2 cpio gzip \
    grub2-pc grub2-efi-x64 xorriso \
    wget curl \
    qemu-system-x86
```

---

## 3. Quick Start

```bash
git clone https://github.com/KoHaku743/fusionos.git
cd fusionos
./build.sh
```

The final ISO is written to `build/fusionos.iso`.

To boot it immediately in QEMU:

```bash
./build.sh test
# or manually:
qemu-system-x86_64 -m 512M -cdrom build/fusionos.iso -boot d
```

---

## 4. Step-by-Step Build

The automated `build.sh` script runs all steps in order.  Each step can also
be run individually:

```bash
./build.sh kernel       # Step 1
./build.sh busybox      # Step 2
./build.sh userspace    # Step 3
./build.sh initramfs    # Step 4
./build.sh iso          # Step 5
```

### 4.1 Kernel

**Downloads** Linux 6.6 LTS from kernel.org, applies the configuration from
`kernel/fusionos.config`, and builds a compressed `bzImage`.

```bash
# Manual equivalent
wget -O build/linux-6.6.tar.xz \
    https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.tar.xz
tar -xf build/linux-6.6.tar.xz -C build/
cp kernel/fusionos.config build/linux-6.6/.config
make -C build/linux-6.6 olddefconfig
make -C build/linux-6.6 -j$(nproc) bzImage
cp build/linux-6.6/arch/x86/boot/bzImage iso/boot/vmlinuz
```

Key kernel features enabled by `kernel/fusionos.config`:

- `CONFIG_IA32_EMULATION=y` — run 32-bit ELF binaries natively
- `CONFIG_BINFMT_MISC=y` — register custom binary handlers for Wine/Darling
- `CONFIG_USER_NS=y`, `CONFIG_PID_NS=y` — namespaces for compat containers
- `CONFIG_OVERLAY_FS=y` — overlay filesystem for container layers
- VirtIO drivers — QEMU/KVM guest support
- `CONFIG_ISO9660_FS=y` — boot directly from ISO/CD-ROM

### 4.2 BusyBox

Provides `/bin/sh`, `/bin/mdev`, and over 300 other Unix utilities inside the
initramfs.  Built as a **statically linked** single binary.

```bash
# Manual equivalent
wget -O build/busybox-1.36.1.tar.bz2 \
    https://busybox.net/downloads/busybox-1.36.1.tar.bz2
tar -xf build/busybox-1.36.1.tar.bz2 -C build/
make -C build/busybox-1.36.1 defconfig
echo "CONFIG_STATIC=y" >> build/busybox-1.36.1/.config
make -C build/busybox-1.36.1 -j$(nproc)
make -C build/busybox-1.36.1 CONFIG_PREFIX=build/initramfs install
```

### 4.3 Userspace Components

All FusionOS-specific binaries are compiled as **static executables** so the
initramfs needs no shared library infrastructure.

```bash
CC=gcc
CFLAGS="-O2 -Wall -Wextra -static"

# Initramfs /init (no printf, just write(2))
$CC $CFLAGS -o build/initramfs/init      src/init/initramfs_init.c

# PID-1 /sbin/init
$CC $CFLAGS -o build/initramfs/sbin/init src/init/init.c

# fsh shell
$CC $CFLAGS -o build/initramfs/bin/fsh   \
    src/shell/shell.c src/compat/detect.c

# Universal launcher
$CC $CFLAGS -o build/initramfs/usr/bin/fusion-run \
    src/compat/fusion_run.c src/compat/detect.c

# Package manager
$CC $CFLAGS -o build/initramfs/usr/bin/fusion-pkg \
    src/pkg/fusion_pkg.c

# System monitor
$CC $CFLAGS -o build/initramfs/usr/bin/fusion-monitor \
    src/monitor/monitor.c

# Binary detection test tool (build directory only)
$CC $CFLAGS -o build/detect_test \
    src/compat/detect_test.c src/compat/detect.c
```

### 4.4 Initramfs

The initramfs is a compressed CPIO archive that the kernel extracts into a
RAM-backed filesystem at early boot.

```bash
# Build the directory tree first (build.sh does this automatically)
mkdir -p build/initramfs/{bin,sbin,usr/bin,usr/sbin,lib,lib64,
                          proc,sys,dev,tmp,newroot,etc,
                          var/lib/fusion-pkg,var/cache/fusion-pkg}

# Populate /etc
echo "fusionos"  > build/initramfs/etc/hostname
cat > build/initramfs/etc/passwd << 'EOF'
root:x:0:0:root:/root:/bin/fsh
EOF

# Create the cpio archive
cd build/initramfs
find . | cpio -o -H newc | gzip -9 > ../../../iso/boot/initrd
cd -
```

The resulting `iso/boot/initrd` is referenced by the GRUB kernel command line.

### 4.5 ISO Assembly

`grub-mkrescue` creates a hybrid ISO/USB image with an embedded GRUB2
bootloader that can boot from both BIOS (via `grub-pc-bin`) and UEFI (via
`grub-efi-amd64-bin`).

```bash
# Copy optional GRUB unicode font
cp /usr/share/grub/unicode.pf2 iso/boot/grub/fonts/ 2>/dev/null || true

grub-mkrescue -o build/fusionos.iso iso/ -- -volid FUSIONOS
```

The `iso/` directory becomes the root of the ISO filesystem.  GRUB reads
`/boot/grub/grub.cfg` from the ISO at boot time.

---

## 5. Component Reference

### 5.1 Binary Detection Library (detect)

**Files:** `src/compat/detect.h`, `src/compat/detect.c`

The central library used by `fsh` and `fusion-run` to identify binary files
and select the correct compatibility launcher.

**Supported formats:**

| Format | Magic bytes | Launcher |
|--------|-------------|---------|
| ELF x86-64 | `7f 45 4c 46` + e_machine=62 | *(native)* |
| ELF x86 (i386) | `7f 45 4c 46` + e_machine=3 | *(native, via IA32 emulation)* |
| ELF ARM | `7f 45 4c 46` + e_machine=40 | `/usr/bin/box64` |
| ELF AArch64 | `7f 45 4c 46` + e_machine=183 | `/usr/bin/fex-emu` |
| ELF RISC-V | `7f 45 4c 46` + e_machine=243 | *(none)* |
| PE32 x86 | `4d 5a` (MZ) + PE\0\0 + machine=0x014c | `/usr/bin/wine` |
| PE32+ x64 | `4d 5a` (MZ) + PE\0\0 + machine=0x8664 | `/usr/bin/wine64` |
| PE ARM / ARM64 | `4d 5a` (MZ) | `/usr/bin/wine[64]` |
| Mach-O 32/64 | `CE FA ED FE` or `CF FA ED FE` variants | `/usr/bin/darling` |
| Mach-O FAT | `CA FE BA BE` | `/usr/bin/darling` |

**API:**

```c
#include "detect.h"

FusionBinInfo info;
if (fusion_detect_format("/path/to/binary", &info) == 0) {
    // info.fmt         — BIN_ELF, BIN_PE, BIN_MACHO, BIN_MACHO_FAT
    // info.arch        — ARCH_X86, ARCH_X86_64, ARCH_ARM, ARCH_ARM64, …
    // info.bits        — 32 or 64
    // info.launcher    — "/usr/bin/wine" etc., or NULL for native
    // info.description — human-readable string
}
```

**Test tool:**

```bash
build/detect_test /bin/ls
build/detect_test notepad.exe
build/detect_test SomeApp.app/Contents/MacOS/SomeApp
```

### 5.2 Initramfs Init

**File:** `src/init/initramfs_init.c`  
**Installed at:** `/init` inside the initramfs

Runs as PID 1 immediately after the kernel decompresses the initramfs.
Responsibilities:

1. Mount `/proc`, `/sys`, `/dev` (devtmpfs)
2. Run `mdev -s` to populate `/dev` with device nodes
3. Search for the root block device (`/dev/vda`, `/dev/sda`, `/dev/sr0`, …)
4. Mount the found device at `/newroot`
5. Call `switch_root` to exec `/sbin/init` from the new root

If no root device is found, it drops into an emergency BusyBox shell.

### 5.3 Main Init (PID 1)

**File:** `src/init/init.c`  
**Installed at:** `/sbin/init`

PID-1 process after `switch_root`.  Responsibilities:

- Mount `/proc`, `/sys`, `/dev`, `/tmp`
- Set default environment (`PATH`, `HOME`, `TERM`, `PS1`)
- Install `SIGCHLD` handler to reap orphan processes
- Ignore `SIGTERM` / `SIGINT`
- Spawn `fsh` in a loop; restart it if it exits

### 5.4 FusionOS Shell (fsh)

**File:** `src/shell/shell.c`  
**Installed at:** `/bin/fsh`

A minimal interactive shell tailored for FusionOS.

**Built-in commands:**

| Command | Description |
|---------|-------------|
| `help` | List available commands |
| `exit` | Exit the shell |
| `cd <dir>` | Change working directory |
| `pwd` | Print working directory |
| `ls [dir]` | List directory contents |
| `history` | Show command history |
| `layers` | List active compatibility layers |
| `compat <file>` | Detect binary format of a file |
| `run <prog> [args]` | Run with automatic compat-layer routing |
| `install <pkg>` | Install a package via `fusion-pkg` |

External commands are looked up in `$PATH` and automatically routed through
the appropriate compatibility launcher based on binary format detection.

### 5.5 fusion-run

**File:** `src/compat/fusion_run.c`  
**Installed at:** `/usr/bin/fusion-run`

A stand-alone launcher that can be used outside of `fsh`:

```bash
fusion-run /path/to/any/binary [arguments...]
FUSION_DEBUG=1 fusion-run notepad.exe   # verbose launcher info
```

It detects the binary format, selects the launcher, and `execv`s it.

### 5.6 fusion-pkg

**File:** `src/pkg/fusion_pkg.c`  
**Installed at:** `/usr/bin/fusion-pkg`

A lightweight package manager that downloads and extracts tarballs from the
FusionOS package repository.

```bash
fusion-pkg install wine        # download and install wine
fusion-pkg install darling
fusion-pkg list                # show installed packages
fusion-pkg update              # update all installed packages
fusion-pkg remove wine
```

The installed-packages database is stored at `/var/lib/fusion-pkg/installed`.
Downloaded archives are cached under `/var/cache/fusion-pkg/` and removed
after successful extraction.

### 5.7 fusion-monitor

**File:** `src/monitor/monitor.c`  
**Installed at:** `/usr/bin/fusion-monitor`

A `top`-like system monitor that reads from `/proc`.

```bash
fusion-monitor           # one-shot snapshot
fusion-monitor -c        # continuous (refresh every second)
fusion-monitor -p 20     # show top 20 processes
```

---

## 6. Kernel Configuration

The kernel configuration lives at `kernel/fusionos.config`.  Notable sections:

### Compatibility Layer Support

```ini
CONFIG_IA32_EMULATION=y       # Run 32-bit ELF binaries natively
CONFIG_BINFMT_MISC=y          # Custom binary format handlers (Wine/Darling)
CONFIG_NAMESPACES=y           # Namespace isolation
CONFIG_USER_NS=y              # User namespaces (required by Wine/Darling)
CONFIG_PID_NS=y               # PID namespaces
CONFIG_NET_NS=y               # Network namespaces
CONFIG_CGROUPS=y              # Control groups
CONFIG_OVERLAY_FS=y           # OverlayFS for container layers
```

### Boot / Initramfs

```ini
CONFIG_BLK_DEV_INITRD=y       # Initramfs support
CONFIG_RD_GZIP=y              # gzip-compressed initramfs
CONFIG_DEVTMPFS=y             # Automatic /dev population
CONFIG_DEVTMPFS_MOUNT=y
```

### Storage Drivers

```ini
CONFIG_SATA_AHCI=y            # SATA/AHCI
CONFIG_VIRTIO_BLK=y           # VirtIO block (QEMU/KVM)
CONFIG_BLK_DEV_SR=y           # CD-ROM (ISO boot)
CONFIG_BLK_DEV_NVME=y         # NVMe
CONFIG_USB_STORAGE=y          # USB mass storage
```

### Filesystems

```ini
CONFIG_ISO9660_FS=y           # ISO 9660 (CD-ROM / ISO image)
CONFIG_EXT4_FS=y              # ext4 root filesystem
CONFIG_SQUASHFS=y             # SquashFS (live media)
CONFIG_TMPFS=y                # tmpfs for /tmp and initramfs
CONFIG_FUSE_FS=y              # FUSE for Darling macOS layer
```

---

## 7. GRUB2 Boot Menu

`iso/boot/grub/grub.cfg` provides four boot entries:

| Entry | Kernel parameters | Purpose |
|-------|-------------------|---------|
| **FusionOS** | `quiet console=tty0 console=ttyS0,115200` | Normal boot |
| **FusionOS (verbose)** | `console=tty0 console=ttyS0,115200` | Debug boot |
| **FusionOS (serial console)** | `quiet console=ttyS0,115200` | Headless / serial-only |
| **Memory Test (Memtest86+)** | — | RAM diagnostics |

Reboot and Power Off entries are also included.

All entries pass `init=/sbin/init` to override the default init path.

---

## 8. ISO Directory Structure

After a full build the `iso/` tree looks like:

```
iso/
└── boot/
    ├── vmlinuz              # Compressed Linux 6.6 kernel (bzImage)
    ├── initrd               # gzip-compressed CPIO initramfs
    ├── memtest86+.bin       # (optional) Memtest86+ binary
    └── grub/
        ├── grub.cfg         # GRUB2 boot menu
        └── fonts/
            └── unicode.pf2  # (optional) GRUB unicode font
```

GRUB2 module files are embedded directly into the ISO by `grub-mkrescue` and
do not appear as visible files.

---

## 9. Testing the ISO

### QEMU (recommended for CI / development)

```bash
# Basic BIOS boot, 512 MB RAM, graphical output
qemu-system-x86_64 -m 512M -cdrom build/fusionos.iso -boot d

# Headless serial console
qemu-system-x86_64 -m 512M -cdrom build/fusionos.iso \
    -nographic -serial stdio -no-reboot

# UEFI boot (requires OVMF)
qemu-system-x86_64 -m 512M -cdrom build/fusionos.iso \
    -bios /usr/share/ovmf/OVMF.fd -boot d

# With KVM acceleration (Linux host only)
qemu-system-x86_64 -enable-kvm -m 512M -cdrom build/fusionos.iso
```

Using `build.sh test` runs a 30-second headless QEMU session and prints early
serial output to stdout.

### Physical media

```bash
# Write to USB (replace /dev/sdX with your device)
sudo dd if=build/fusionos.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

The hybrid ISO format produced by `grub-mkrescue` is directly bootable from
a USB stick without any additional partitioning.

---

## 10. Customisation

### Changing the kernel configuration

```bash
make -C build/linux-6.6 menuconfig
cp build/linux-6.6/.config kernel/fusionos.config
./build.sh kernel initramfs iso
```

### Adding packages to the default image

Place tarballs or files in `build/initramfs/` before running
`./build.sh initramfs`.  Everything in that directory tree is included in the
initramfs.

For example, to bundle `htop`:

```bash
mkdir -p build/initramfs/usr/bin
cp /usr/bin/htop build/initramfs/usr/bin/
./build.sh initramfs iso
```

### Adjusting boot parameters

Edit `iso/boot/grub/grub.cfg` and add or change kernel parameters on the
`linux` lines, then rebuild the ISO:

```bash
./build.sh iso
```

### Parallel build jobs

```bash
JOBS=16 ./build.sh
```

### Custom compiler

```bash
CC=clang ./build.sh userspace
```

### Clean rebuild

```bash
./build.sh clean
./build.sh
```
