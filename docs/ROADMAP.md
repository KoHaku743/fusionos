# FusionOS — Phased Development Roadmap

## Phase 0 — Foundation (current)

**Goal**: Bootable minimal system with core userspace components.

### Completed
- [x] Linux 6.6 LTS kernel configuration (gaming-tuned: PREEMPT, HZ=1000, DRM modules)
- [x] GRUB2 bootloader configuration with multiple boot entries
- [x] Initramfs init (`initramfs_init.c`) — device probing, root mount, switch_root
- [x] Init (PID 1) (`init.c`) — filesystem mounts, environment, shell supervisor
- [x] `fsh` shell — built-in commands, compat routing, command history
- [x] `fusion-run` — universal binary launcher (ELF/PE/Mach-O detection)
- [x] `fusion-pkg` — package manager CLI (install/remove/list/search/info/update)
- [x] `fusion-monitor` — real-time CPU, memory, process monitor
- [x] Binary format detection library (`detect.c`) — ELF, PE32/PE32+, Mach-O

### Remaining Phase 0 work
- [ ] Build and boot a minimal ISO image with QEMU
- [ ] BusyBox integration for standard Unix utilities
- [ ] `udev` / `mdev` rules for GPU and input devices
- [ ] Basic `/etc` skeleton (passwd, group, fstab, hostname)

---

## Phase 1 — MVP Desktop (month 2–4)

**Goal**: A usable desktop environment with package management, gaming basics, and
        a working compatibility layer via Wine and Box64.

### Shell & Terminal
- [ ] Replace `fsh` with a bash-compatible shell (embed Busybox ash or bash)
- [ ] Add readline-style line editing and tab completion
- [ ] Terminal emulator (Alacritty or foot) as the default

### Display & GUI
- [ ] Integrate Weston (Wayland compositor) as the default desktop
- [ ] Basic desktop panel / launcher (waybar or wf-shell)
- [ ] Theming support via GTK4 + a default FusionOS theme
- [ ] Font rendering (fontconfig + FreeType)
- [ ] Screen resolution and multi-monitor configuration tool

### Package Management
- [ ] Define `.fpkg` binary package format (header + compressed payload + manifest)
- [ ] Implement `fusion-pkg` native backend (no delegation to apt/pacman)
- [ ] Package signing with Ed25519 keys
- [ ] Web-hosted package repository with index and CDN
- [ ] GUI frontend for `fusion-pkg` (Qt/GTK application)

### Gaming Basics
- [ ] Steam integration (install via `fusion-pkg install steam`)
- [ ] Lutris integration for non-Steam game management
- [ ] DXVK + VKD3D-Proton bundled as default Wine components
- [ ] Gamemode daemon — dynamic CPU/GPU governor tuning on game launch
- [ ] Gamepad configuration utility (SDL2-based)
- [ ] MangoHud overlay for FPS / GPU stats

### Compatibility Layer (Linux)
- [ ] Flatpak runtime support for sandboxed Linux app distribution
- [ ] AppImage support via FUSE
- [ ] Snap support (optional)

### Compatibility Layer (Windows)
- [ ] Ship Wine + Wine64 in base image
- [ ] `binfmt_misc` registration for PE binaries → auto-route through Wine
- [ ] Proton-style Wine prefix management per-game

---

## Phase 2 — Stable Desktop (month 5–9)

**Goal**: Daily-driver quality; full theming, stable gaming, and a polished UX.

### Desktop Environment
- [ ] Migrate to KWin (KDE Plasma) or sway for full compositing and theme engine
- [ ] System settings application (display, audio, input, network)
- [ ] Notification daemon (mako or dunst)
- [ ] File manager (Dolphin or Thunar)
- [ ] Screenshot / screen recording tool

### Performance
- [ ] Kernel patch set: `BORE` or `TT` scheduler for better interactivity
- [ ] zram swap enabled by default (lz4 compression)
- [ ] Read-ahead tuning for game asset streaming
- [ ] NVMe power management profiles
- [ ] CPU core isolation support (`isolcpus`) for dedicated game core(s)

### GPU & Drivers
- [ ] Automated Mesa version management (update via `fusion-pkg`)
- [ ] AMD ROCm compute stack (GPU compute for AI / streaming tools)
- [ ] NVIDIA proprietary driver installer (`fusion-pkg install nvidia-driver`)
- [ ] VRR / FreeSync / G-Sync configuration in display settings

### Gaming Platform
- [ ] FusionOS Game Hub — curated native Linux game catalogue
- [ ] Cloud save integration layer
- [ ] Discord RPC and overlay support
- [ ] Anti-cheat compatibility research and documentation

### Security
- [ ] Mandatory access control (AppArmor profiles for system daemons)
- [ ] Secure boot support (sign kernel and bootloader with MOK)
- [ ] Automatic security updates via `fusion-pkg update --security`
- [ ] Firewall (nftables) with a simple GUI frontend

---

## Phase 3 — Advanced Features (month 10–18)

**Goal**: Production-ready OS with full compatibility support and ecosystem.

### macOS Compatibility (Future Milestone)
> Full macOS app support is technically complex and is scoped here as a Phase 3
> milestone.  The current binary detection layer routes Mach-O binaries to
> Darling, but a proper implementation requires:
- [ ] Evaluate Darling stability on Linux 6.6 (build, test, upstream patches)
- [ ] `binfmt_misc` registration for Mach-O magic bytes
- [ ] Darwin syscall shim (subset covering common frameworks)
- [ ] macOS framework stubs (Foundation, AppKit, CoreFoundation)
- [ ] Metal → Vulkan translation layer (for GPU-accelerated macOS apps)
- [ ] **Decision point**: If Darling proves insufficiently stable, ship a
      container-based approach (macOS VM via QEMU/KVM with virtio-gpu) instead

### Native Sandboxing
- [ ] FusionOS native app sandbox (Landlock + seccomp + namespaces)
- [ ] Per-app storage quota and network policy
- [ ] Hardware attestation (TPM 2.0 integration)

### Ecosystem
- [ ] FusionOS SDK for native app development (C/C++, Rust, Python, Go)
- [ ] Developer documentation portal
- [ ] Automated CI/CD for ISO builds (GitHub Actions / self-hosted runner)
- [ ] Live ISO with installer (calamares-based)
- [ ] OTA system updates with A/B partition support
- [ ] FusionOS hardware compatibility database

### ARM64 Port
- [ ] Evaluate porting to ARM64 (Raspberry Pi 5, Apple Silicon via Asahi Linux base)
- [ ] Adjust gamepad and GPU driver assumptions for ARM SoCs

---

## Dependency Graph

```
Phase 0 (Foundation)
    │
    ├──▶ Phase 1 (MVP Desktop)
    │       │
    │       ├──▶ Phase 2 (Stable Desktop)
    │       │           │
    │       │           └──▶ Phase 3 (Advanced / macOS compat)
    │       │
    │       └──▶ Phase 2 (Gaming Platform) ──▶ Phase 3 (Game Hub)
    │
    └──▶ Phase 1 (Compatibility: Wine/Box64) ──▶ Phase 3 (macOS/Darling)
```

---

## Known Blockers

| Blocker | Impact | Mitigation |
|---|---|---|
| Darling instability | macOS compat unreliable | Defer to Phase 3; use container VM fallback |
| Anti-cheat (EAC, BattlEye) | Some online games unplayable | Work with vendors; rely on kernel-level solutions |
| NVIDIA proprietary driver | Requires out-of-tree DKMS | Ship installer; document secure boot MOK enrollment |
| `.fpkg` format design | Phase 1 pkg mgr depends on it | Prototype alongside MVP; finalise before Phase 2 |
| Wayland game compatibility | Some engines default to X11 | Ship XWayland + gamescope as fallback |
