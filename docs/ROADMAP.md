# FusionOS — Phased Development Roadmap (DOS-First)

The FusionOS roadmap is structured around the DOS-core identity: each phase
builds upward from the DOS foundation, never breaking backward compatibility
with the DOS environment below it.

---

## Phase 0 — DOS Foundation (current)

**Goal**: A bootable system that presents a `C:\>` prompt and behaves like
         MS-DOS.  Everything else is optional from here.

### Completed
- [x] Linux 6.6 LTS kernel config — HAL layer (PREEMPT, HZ=1000, DRM, FAT32, gamepad)
- [x] GRUB2 bootloader with multi-entry boot menu
- [x] Initramfs init (`initramfs_init.c`) — device probe, FAT32 root mount, switch_root
- [x] Init PID 1 (`init.c`) — filesystem mounts, COMMAND.COM spawn
- [x] **COMMAND.COM++** (`fsh` / `shell.c`) — DOS shell with:
  - Classic commands: `DIR`, `CD`, `MD`, `RD`, `COPY`, `DEL`, `REN`, `TYPE`, `CLS`
  - Environment: `SET`, `PATH`, `PROMPT`, `ALIAS`
  - Batch scripting: `.BAT` files, `@ECHO OFF`, `GOTO :label`, `IF`/`IF NOT`, `CALL`
  - `%VARIABLE%` and `%0`–`%9` expansion
  - Session: `VER`, `DATE`, `TIME`, `PAUSE`, `DOSKEY` (history)
  - Modern: `COMPAT`, `RUN`, `LAYERS`, `INSTALL`, `SYSINFO`
- [x] `C:\AUTOEXEC.BAT` template (`src/shell/autoexec.bat`)
- [x] `C:\CONFIG.SYS` template (`src/shell/config.sys`)
- [x] Binary format detection (`detect.c`) — ELF / PE32+ / Mach-O / FAT
- [x] Universal launcher (`fusion-run`) — routes to Wine, Darling, Box64, FEX-emu
- [x] `fusion-pkg` package manager — install / remove / list / search / info / update
- [x] `fusion-monitor` — real-time system monitor
- [x] FAT32 filesystem driver (`fat.c`) — FAT12/16/32 read + LFN support
- [x] `fat-info` CLI tool — FAT volume inspector

### Remaining Phase 0 work
- [ ] Boot a minimal ISO in QEMU (`make iso`)
- [ ] BusyBox integration — `ls`, `cp`, `cat` available as fallbacks
- [ ] `udev` rules for GPU, USB, and input devices
- [ ] `/etc/fusionos/config.sys` and `autoexec.bat` installed to rootfs
- [ ] `C:\DOS` skeleton populated with `fusion-pkg`, `fusion-run`, `fat-info`

---

## Phase 1 — DOS MVP (Month 2–4)

**Goal**: A complete, daily-usable DOS environment.  Gaming works.
         The GUI is available but opt-in.

### Shell (`COMMAND.COM++`)
- [ ] Pipe support: `DIR | MORE`, `TYPE file.txt | FIND "word"`
- [ ] I/O redirection: `DIR > out.txt`, `ECHO input | COMMAND`
- [ ] `MORE` — pager for long output
- [ ] `FIND` — in-line text search (equivalent of `grep`)
- [ ] `FOR %%V IN (set) DO cmd` — full loop implementation
- [ ] `CHOICE` — interactive yes/no prompt for batch files
- [ ] `ERRORLEVEL` support in all batch conditionals
- [ ] Arrow-key history navigation (readline-style line editing)
- [ ] Tab completion for filenames and commands

### FAT32 Filesystem (`fat.c`)
- [ ] FAT32 write support: cluster allocation, file creation, truncation
- [ ] `fat_create()` and `fat_unlink()` full implementation
- [ ] Directory creation and removal
- [ ] File attribute setting (read-only, hidden, archive)
- [ ] `CHKDSK C:` command — FAT consistency check

### Package Manager (`fusion-pkg`)
- [x] Define `.fpkg` package format:
  - Header: name, version, arch, checksum (SHA-256)
  - File entry table: DOS destination path, payload offset, mode
  - Pre/post-install scripts (`.BAT` format)
  - SHA-256 trailer covering all preceding bytes
- [x] Native backend (no delegation to apt/pacman):
  - `fusion-pkg pack <manifest> <files_dir> <out.fpkg>` — create packages
  - `fusion-pkg verify <file.fpkg>` — check magic + SHA-256
  - `fusion-pkg install <file.fpkg>` — extract, run scripts, update DB
  - Legacy fallback: `fusion-pkg install <name>` still delegates to system pkg mgr
- [ ] Package signing with Ed25519
- [ ] Initial FusionOS package repository with core tools
- [ ] `fusion-pkg upgrade` — upgrade installed packages

### Gaming Basics
- [ ] `fusion-pkg install wine` — installs Wine to `C:\DOS\WINE`
- [ ] `fusion-pkg install dxvk` — installs DXVK alongside Wine
- [ ] `binfmt_misc` registration for `.exe` → automatic Wine dispatch
- [ ] Steam launcher: `C:\GAMES\STEAM\steam.exe` (via Wine)
- [ ] Gamemode daemon: sets CPU governor to `performance` on game launch
- [ ] `SYSINFO` command — comprehensive hardware report
- [ ] MangoHud overlay: `fusion-pkg install mangohud`

### Compatibility (Linux)
- [ ] AppImage support: mount and execute `.AppImage` files transparently
- [ ] Flatpak: `fusion-pkg install flatpak` optional runtime

### GUI (opt-in)
- [ ] Weston Wayland compositor: `fusion-pkg install desktop`
- [ ] `START DESKTOP` command from `COMMAND.COM` launches Weston
- [ ] Minimal launcher panel (waybar or wf-shell)
- [ ] Window close returns to `C:\>` prompt

---

## Phase 2 — Stable DOS Desktop (Month 5–9)

**Goal**: FusionOS is the daily driver. Gaming is seamless. The GUI is polished.

### Shell
- [ ] Full job control: `CTRL+Z` (suspend), `fg`, `bg`
- [ ] Script functions (`GOSUB`/`RETURN` in `.BAT`)
- [ ] `PUSHD` / `POPD` — directory stack
- [ ] Colour output: `COLOR 0A` sets background/foreground like classic DOS
- [ ] `SUBST D: C:\GAMES` — virtual drive letters

### FAT32 + Filesystem
- [ ] exFAT driver for drives > 2 TB
- [ ] Long filename creation in `fusion-pkg` and `COMMAND.COM`
- [ ] `DEFRAG C:` — FAT32 defragmentation tool
- [ ] RAM disk: `RAMDRIVE 64M D:` creates a fast in-memory drive

### Performance
- [ ] `BORE` or `TT` scheduler patch for better gaming interactivity
- [ ] `ISOLCPU` support: dedicate CPU cores to a game process
- [ ] NVMe power management profiles per workload
- [ ] `TUNEUP` command: apply all gaming optimisations in one step

### Gaming
- [ ] FusionOS Game Hub: curated native catalogue, installed via `fusion-pkg`
- [ ] GOG Galaxy integration (Wine)
- [ ] Discord overlay (native)
- [ ] VRR/FreeSync/G-Sync configuration (`DISPLAY CONFIG`)
- [ ] NVIDIA proprietary driver: `fusion-pkg install nvidia-driver`

### GUI (polished)
- [ ] KWin (KDE Plasma) or sway with FusionOS theme
- [ ] System settings application (display, audio, input, network)
- [ ] File manager that understands DOS paths (`C:\`)
- [ ] `EDIT C:\AUTOEXEC.BAT` — in-terminal text editor (QBasic/EDIT style)

### Security
- [ ] AppArmor profiles for Wine and Darling sandboxes
- [ ] Secure Boot (sign GRUB2 + kernel with MOK)
- [ ] `fusion-pkg update --security` — security-only updates

---

## Phase 3 — Advanced / Custom Kernel (Month 10–24)

**Goal**: Replace the Linux HAL with a native FusionOS kernel.  Full macOS
         app support.  ARM64 port.

### FusionDOS Kernel (custom)
- [ ] Design 64-bit protected-mode DOS kernel (`kernel/fusionos_kernel/`)
  - Interrupt-driven, preemptive multi-tasking
  - Native FAT32 + exFAT VFS layer
  - Custom syscall ABI (`INT 0x80` DOS-style + modern 64-bit fast-path)
  - ELF + PE32+ + Mach-O binary loader (via `binfmt_misc` equivalent)
  - DRM/KMS driver interface (compatible with Mesa)
  - USB, NVMe, AHCI drivers
- [ ] Bootable ISO using the custom kernel (replace Linux HAL)
- [ ] Compatibility shim: run existing FusionOS Phase 0-2 userspace unmodified

### macOS App Support (Mach-O / Darwin)
> **Decision gate**: proceed only if Darling reaches sufficient stability on
> the custom kernel.  If not, ship a macOS VM (QEMU/KVM + virtio-gpu) instead.

- [ ] Evaluate Darling on the custom kernel
- [ ] Darwin syscall shim (Foundation/AppKit subset)
- [ ] `binfmt_misc`-equivalent Mach-O loader registration
- [ ] Metal → Vulkan translation layer
- [ ] **Fallback**: macOS VM option documented and packaged

### Ecosystem
- [ ] FusionOS SDK: C/C++ + Rust + Python + Go, targeting the custom kernel ABI
- [ ] Developer portal with documentation
- [ ] ISO installer (calamares-based) with FAT32 partition setup
- [ ] OTA updates: A/B partition scheme, atomic rollback
- [ ] Hardware compatibility database

### ARM64 Port
- [ ] Evaluate ARM64 target (Raspberry Pi 5, Apple Silicon via Asahi base)
- [ ] Adapt gamepad and GPU driver assumptions for ARM SoCs
- [ ] Cross-compile toolchain for ARM64 FusionOS binaries

---

## Dependency Graph

```
Phase 0 (DOS Foundation)
    │
    ├──► Phase 1 (DOS MVP)
    │       │
    │       ├──► Phase 2 (Stable Desktop)
    │       │           │
    │       │           └──► Phase 3 (Custom Kernel + macOS)
    │       │
    │       └──► Phase 2 (Gaming Platform)
    │                   │
    │                   └──► Phase 3 (Game Hub + full anti-cheat)
    │
    └──► Phase 1 (.fpkg format) ──► Phase 2 (full package ecosystem)
```

---

## Blockers and Risks

| Blocker | Impact | Mitigation |
|---|---|---|
| FAT32 4 GiB file limit | Large games (>4 GiB files) can't live on FAT32 | Use exFAT for game drives; Phase 1 deliverable |
| Custom kernel complexity | Phase 3 is 12-24 months of kernel work | Linux HAL is fully functional; custom kernel is additive |
| Darling instability | macOS compat unreliable | Gate on Phase 3 evaluation; VM fallback |
| Anti-cheat (EAC, BattlEye) | Major online games unplayable | Kernel-level solution required; tracking [EAC Linux support](https://store.steampowered.com/news/app/1091500/view/3144817738498852814), Proton GE patches, and the `kernel_params` anti-cheat kernel work in progress |
| NVIDIA proprietary driver | Out-of-tree DKMS; breaks on kernel updates | Ship installer + doc; pin HAL kernel version per stable release |
| Wine DirectX coverage | ~90% compat; edge cases in latest games | Bundle Proton-style patches; track GE-Proton |
| Wayland game compat | Some engines still default to X11 | Ship gamescope wrapper; document XWayland path |
