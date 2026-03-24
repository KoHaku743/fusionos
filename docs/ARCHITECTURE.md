# FusionOS — Architecture Specification

## 1. Overview

FusionOS is a gaming-first desktop operating system built on a Linux 6.6 LTS kernel base.
Its design philosophy combines the **simplicity and speed** of MS-DOS with the
**transparency and control** of Linux, while adding first-class support for modern GPU
workloads, low-latency gaming, and cross-platform binary compatibility.

---

## 2. System Stack (boot to userspace)

```
┌─────────────────────────────────────────────────────────┐
│                     Applications                        │
│  Games · GUI Apps · CLI Tools · Compatibility Layer     │
├─────────────────────────────────────────────────────────┤
│                  FusionOS Userspace                      │
│  fsh  │  fusion-pkg  │  fusion-run  │  fusion-monitor   │
├──────────────┬──────────────────────┬────────────────────┤
│  Display     │  Compatibility       │  Gaming            │
│  Server      │  Layer               │  Services          │
│  (Wayland /  │  Wine / Darling /    │  Scheduler·GPU     │
│   Weston)    │  Box64  / FEX-emu    │  Gamepad·Overlay   │
├─────────────────────────────────────────────────────────┤
│                    Init (PID 1)                          │
│              src/init/init.c                             │
├─────────────────────────────────────────────────────────┤
│              Linux 6.6 LTS Kernel                        │
│   PREEMPT · HZ=1000 · DRM (AMDGPU/i915/nouveau)         │
│   ext4 · tmpfs · overlayfs · io_uring · namespaces      │
├─────────────────────────────────────────────────────────┤
│              Initramfs (BusyBox + mdev)                  │
│              src/init/initramfs_init.c                   │
├─────────────────────────────────────────────────────────┤
│              GRUB2 Bootloader                            │
│              iso/boot/grub/grub.cfg                      │
├─────────────────────────────────────────────────────────┤
│              UEFI / BIOS Firmware                        │
└─────────────────────────────────────────────────────────┘
```

---

## 3. Technology Choices

### 3.1 Kernel

| Decision | Choice | Rationale |
|---|---|---|
| Kernel base | Linux 6.6 LTS | Mature, well-supported, rich driver ecosystem; avoids microkernel latency |
| Preemption model | `CONFIG_PREEMPT` (full preemption) | Reduces worst-case latency vs `PREEMPT_NONE`; better frame pacing |
| Timer frequency | `CONFIG_HZ_1000` | 1 ms tick granularity reduces input latency and scheduler jitter |
| Scheduler | CFS + `SCHED_AUTOGROUP` | Groups interactive vs background work; keeps game threads responsive |
| CPU governor | `performance` (default) | Eliminates frequency-scaling latency during gameplay |
| GPU drivers | DRM modules (AMDGPU, i915, Nouveau) | Native kernel-mode drivers; Vulkan via Mesa userspace |
| Filesystem | ext4 (primary) | Battle-tested, fast fsync, widely supported; ZFS optional via DKMS |
| I/O interface | io_uring | Lowest syscall overhead for async I/O; critical for asset streaming |
| Namespaces + cgroups | Enabled | Required for Wine/container compat layers |

### 3.2 Boot Sequence

1. **UEFI/BIOS** loads GRUB2 from the ISO/ESP partition
2. **GRUB2** decompresses the kernel and passes the initramfs
3. **initramfs init** (`src/init/initramfs_init.c`) mounts `/proc`, `/sys`, `/dev`,
   runs `mdev -s` to populate device nodes, probes the root block device, and
   performs `switch_root` to the real rootfs
4. **init (PID 1)** (`src/init/init.c`) mounts remaining pseudo-filesystems,
   sets environment, and spawns `fsh`

### 3.3 Shell — `fsh`

- **Source**: `src/shell/shell.c`
- A lightweight C shell with built-in commands: `cd`, `pwd`, `ls`, `history`,
  `compat`, `run`, `install`, `layers`
- Integrates with the compat layer: any executable passed through `run` or
  typed directly is detected and routed to the appropriate launcher
- Designed as an MVP; a future milestone replaces it with a bash/zsh-compatible
  shell or embeds musl libc + readline for proper line editing

### 3.4 Package Manager — `fusion-pkg`

- **Source**: `src/pkg/fusion_pkg.c`
- Unified CLI frontend: `install`, `remove`, `list`, `search`, `info`, `update`
- Delegates to the host package manager (`apt-get`, `pacman`, `dnf`) for the
  actual package operations during the bootstrapping phase
- Maintains its own installed-package database at `/var/lib/fusion-pkg/installed.db`
- Future: replaces delegation with a native binary package format (`.fpkg`) with
  cryptographic signatures and a content-addressed cache in `/var/cache/fusion-pkg`

### 3.5 Binary Compatibility — `detect` + `fusion-run`

- **Source**: `src/compat/detect.c`, `src/compat/fusion_run.c`
- `fusion_detect_format()` reads magic bytes and parses ELF / PE / Mach-O headers
  to identify the binary's format and architecture
- `fusion-run` uses this to transparently dispatch:

| Binary type | Launcher |
|---|---|
| ELF x86 / x86-64 | Native kernel execution |
| ELF ARM (32-bit) | Box86 |
| ELF AArch64 | FEX-emu |
| PE x86 (Windows) | Wine |
| PE x86-64 (Windows) | Wine64 |
| Mach-O (macOS) | Darling |
| Mach-O FAT (macOS) | Darling |

### 3.6 Display Server

- **MVP**: Weston (reference Wayland compositor) for minimal overhead
- **Full desktop**: KWin (KDE) or sway (i3-compatible Wayland) for full
  compositing + theming
- Wayland-native to avoid the X11→Wayland translation overhead; XWayland
  available for legacy apps
- Gaming mode: compositor tear-control protocol / VRR (variable refresh rate)
  via KMS/DRM direct flip

### 3.7 GPU & Vulkan

- Kernel-side: DRM modules (AMDGPU, i915, Nouveau) built as loadable modules
- Userspace: Mesa (radv for AMD, ANV for Intel, Nouveau for NVIDIA open) provides
  Vulkan ICD and OpenGL
- DXVK translates DirectX 9/10/11 to Vulkan; VKD3D-Proton translates DirectX 12
- NVIDIA proprietary driver: supported via DKMS and standard Linux interfaces

---

## 4. Directory Layout

```
fusionos/
├── build.sh               # Shell build script
├── Makefile               # GNU Make build system
├── README.md              # Project overview
├── docs/
│   ├── ARCHITECTURE.md    # This document
│   └── ROADMAP.md         # Phased development roadmap
├── iso/
│   └── boot/
│       └── grub/
│           └── grub.cfg   # GRUB2 bootloader config
├── kernel/
│   └── fusionos.config    # Linux kernel .config
└── src/
    ├── compat/
    │   ├── detect.h        # Binary format detection API
    │   ├── detect.c        # ELF / PE / Mach-O detection
    │   ├── detect_test.c   # CLI test utility
    │   └── fusion_run.c    # Universal binary launcher
    ├── init/
    │   ├── init.c          # PID 1 init process
    │   └── initramfs_init.c # Initramfs early init
    ├── monitor/
    │   └── monitor.c       # System resource monitor
    ├── pkg/
    │   └── fusion_pkg.c    # Package manager CLI
    └── shell/
        └── shell.c         # fsh — FusionOS shell
```

---

## 5. Gaming Architecture

### 5.1 Kernel-level optimisations

- **Full preemption** (`CONFIG_PREEMPT`) keeps game threads from being starved
  by kernel code paths
- **HZ=1000** provides 1 ms timer resolution, reducing frame-time variance
- **High-resolution timers** (`CONFIG_HIGH_RES_TIMERS`) enable nanosecond-precision
  sleep/wake for game loops
- **CPU frequency governor** set to `performance` to eliminate DVFS-induced jitter
- **Transparent hugepages** (madvise mode) reduces TLB pressure in memory-hungry games
- **ZSWAP with LZ4** compresses swap in RAM to reduce disk swap latency

### 5.2 Scheduler tuning

- `SCHED_AUTOGROUP` groups processes by session, preventing background work
  from stealing time from the game
- Game launchers may set `SCHED_FIFO` or `SCHED_RR` with moderate priority for
  audio and input threads
- `gamemode` daemon (future) dynamically applies CPU/GPU governor and cgroup
  settings when a game launches

### 5.3 Gamepad input

- Xbox (XPAD) and PlayStation (HID_SONY) HID drivers built in
- `udev` rules expose `/dev/input/jsX` and `/dev/input/event*` to unprivileged users
- SDL2 / libgamepad provides a unified API across game frameworks

### 5.4 Graphics pipeline (Vulkan / DirectX)

```
Game (Vulkan API)
       │
       ▼
   Mesa (radv / ANV)  ←── DXVK / VKD3D-Proton (for DirectX games)
       │
       ▼
  KMS/DRM kernel driver
       │
       ▼
  Display hardware (HDMI / DP with VRR/FreeSync/G-Sync)
```

---

## 6. Compatibility Layer Architecture

> **Note**: Full macOS syscall emulation is treated as a future milestone (Phase 3).
> The current implementation provides binary *detection* and routing to Darling,
> but does not ship a custom Mach-O loader or Darwin syscall shim.

### 6.1 Current state (MVP)

- `fusion_detect_format()` identifies binaries at the file level
- `fusion-run` routes to external launchers (Wine, Darling, Box64, FEX-emu)
- No custom syscall translation in-tree; relies on the maturity of those projects

### 6.2 Future state (Phase 3)

- Linux app emulation: container runtime (similar to Flatpak) using `overlayfs` +
  user namespaces; no syscall translation needed
- Windows app emulation: Wine integration tightened via Proton-style patches and
  a FusionOS-specific Wine prefix
- macOS app support: Darling integration or a lightweight Mach-O loader using
  `binfmt_misc` to register Mach-O magic bytes with the kernel

---

## 7. Known Trade-offs and Blockers

| Area | Trade-off / Blocker |
|---|---|
| Preemption | `CONFIG_PREEMPT` increases kernel complexity; throughput under heavy I/O may be slightly lower than `PREEMPT_NONE` |
| HZ=1000 | Slightly higher CPU wake-up rate than HZ=250; negligible on modern hardware, measurable on low-power/embedded |
| Wine (DirectX) | Wine is not 100% compatible; some games require Proton-specific patches or specific DXVK versions |
| Darling (macOS) | Darling is experimental and does not support Metal or macOS 13+; macOS compat is best-effort |
| GPU drivers | NVIDIA open-source (Nouveau) has limited performance; the proprietary driver is recommended for NVIDIA users |
| fsh shell | Current fsh is a minimal MVP shell; no readline, no job control, no scripting; must be replaced for production |
| Package format | `fusion-pkg` delegates to the host package manager in Phase 1; a native `.fpkg` format is needed for Phase 2 |
| Wayland compatibility | Some games still require XWayland or a gamescope XWayland wrapper for correct fullscreen behaviour |
