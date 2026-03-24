# FusionOS — Architecture Specification (DOS-First Edition)

## 1. Design Philosophy

FusionOS is **MS-DOS evolved into 2025**, not a Linux distribution with a
retro theme.  Its core identity:

- Boots directly into a COMMAND.COM-like shell — no login manager, no display
  server at boot, no layers of abstraction between you and the machine.
- FAT32 is the native, first-class filesystem on the boot partition.
- The shell (`COMMAND.COM++`) is the primary interface; everything else is
  layered on top of it as optional subsystems.
- Modern features (Wayland GUI, package manager, gaming stack, cross-platform
  compatibility) are **add-on layers**, not prerequisites.

The guiding metaphor: *what would DOS look like if Microsoft had kept
developing it through 2025, borrowing the best ideas from Linux without
abandoning the DOS paradigm?*

---

## 2. System Stack

```
┌────────────────────────────────────────────────────────────────────┐
│                         Applications                               │
│   Games · GUI Apps · DOS Batch Scripts · CLI Tools                 │
├────────────────────────────────────────────────────────────────────┤
│                  Optional Subsystems (layered on DOS)              │
│  ┌────────────┐  ┌────────────┐  ┌─────────────┐  ┌────────────┐  │
│  │  Wayland   │  │  fusion-   │  │  Gaming     │  │  Compat    │  │
│  │  GUI Layer │  │  pkg       │  │  Stack      │  │  Layer     │  │
│  │ (Weston/   │  │ (package   │  │ (DXVK/Wine/ │  │ (Wine/     │  │
│  │  KWin)     │  │  manager)  │  │  Vulkan)    │  │  Darling/  │  │
│  └────────────┘  └────────────┘  └─────────────┘  │  Box64)    │  │
│                                                     └────────────┘  │
├────────────────────────────────────────────────────────────────────┤
│              COMMAND.COM++ (fsh) — DOS-like shell / PID 1 wrapper  │
│    DIR · CD · COPY · TYPE · SET · IF · GOTO · .BAT scripts         │
│    fusion-run (universal binary launcher) · fusion-pkg             │
├────────────────────────────────────────────────────────────────────┤
│                  FusionOS Init (PID 1)                              │
│              src/init/init.c  (spawns COMMAND.COM)                 │
├────────────────────────────────────────────────────────────────────┤
│             Hardware Abstraction Layer (Linux 6.6 LTS kernel)       │
│  PREEMPT · HZ=1000 · DRM (AMD/Intel/Nouveau) · FAT32 · io_uring   │
│  Gamepad (XPAD/HID) · USB · NVMe · TCP/IP                         │
├────────────────────────────────────────────────────────────────────┤
│              Initramfs (BusyBox + mdev)                             │
│              src/init/initramfs_init.c                             │
├────────────────────────────────────────────────────────────────────┤
│              GRUB2 Bootloader                                       │
│              iso/boot/grub/grub.cfg                                │
├────────────────────────────────────────────────────────────────────┤
│              UEFI / BIOS Firmware                                   │
└────────────────────────────────────────────────────────────────────┘
```

---

## 3. Boot Sequence

```
1. UEFI/BIOS
   └─► GRUB2
        └─► Kernel (Linux 6.6 LTS — Hardware Abstraction Layer)
             └─► initramfs init
                  ├─ mount /proc, /sys, /dev
                  ├─ mdev -s  (populate device nodes)
                  ├─ probe root FAT32 partition
                  └─ switch_root
                       └─► PID 1 (init.c)
                            ├─ mount remaining filesystems
                            ├─ parse /etc/fusionos/config.sys  ← DOS CONFIG.SYS equivalent
                            └─► COMMAND.COM (fsh)
                                 ├─ run /etc/fusionos/autoexec.bat  ← DOS AUTOEXEC.BAT
                                 └─► C:\>  (interactive prompt)
```

Key point: the user lands at `C:\>` — a DOS command prompt — by default.
The GUI is an opt-in started with `START DESKTOP` or automatically from
`AUTOEXEC.BAT` if the user configures it.

---

## 4. Technology Choices

### 4.1 Kernel — Hardware Abstraction Layer

FusionOS does **not** expose Linux semantics to the user.  The Linux kernel
is used purely as a hardware driver platform (GPU, storage, network, HID).

| Decision | Choice | Rationale |
|---|---|---|
| Kernel base | Linux 6.6 LTS | Best hardware support, mature GPU drivers, io_uring |
| Preemption | `CONFIG_PREEMPT` (full) | Low input/frame latency for gaming |
| Timer frequency | `CONFIG_HZ_1000` | 1 ms granularity — matches classic DOS timer accuracy |
| CPU governor | `performance` | Eliminates DVFS latency during gameplay |
| GPU | DRM modules (AMDGPU, i915, Nouveau) | Vulkan via Mesa; future direct-metal path |
| Primary filesystem | **FAT32** (`CONFIG_VFAT`) | DOS-native; boot and install partition |
| Secondary filesystem | ext4 | Linux HAL rootfs; hidden from the user |
| I/O | io_uring | Low-syscall async I/O for game asset streaming |

**Why not a custom kernel?**  Writing a full 64-bit protected-mode kernel
from scratch is a multi-year effort and out of scope for Phase 0–1.
The Linux HAL approach lets us ship a real, bootable OS while the custom
kernel is designed and built in parallel (Phase 3+, see ROADMAP.md).

### 4.2 Primary Filesystem — FAT32

FAT32 is the native FusionOS filesystem because:

- Every MS-DOS program expects FAT.  Paths like `C:\GAMES\DOOM` work without
  any translation layer.
- Universally readable by Windows, macOS, and Linux — zero compatibility
  friction for game installs, USB drives, and dual-boot scenarios.
- Simple on-disk format that can be fully understood and debugged without
  special tools.

The `src/fs/fat.c` driver reads FAT12/16/32 volumes directly (supporting Long
Filenames via LFN) and will gain write support in Phase 1.

For large secondary storage (data drives > 2 TB) exFAT is supported; ext4
remains available as the Linux HAL's rootfs, but is never exposed to the
user-facing DOS environment.

### 4.3 Shell — COMMAND.COM++  (`fsh`)

Source: `src/shell/shell.c`

The shell is the **primary user interface**.  Design goals:

- Classic DOS commands: `DIR`, `CD`, `COPY`, `DEL`, `TYPE`, `SET`, `ECHO`, `REM`
- Batch scripting: `.BAT` files, `GOTO`, `IF`/`IF NOT`, `CALL`, `%0`–`%9`
  parameters, `%VARIABLE%` expansion
- Modern additions: `ALIAS`, `DOSKEY` (history), `PROMPT`, named pipes (future)
- Compat extensions: `COMPAT`, `RUN`, `LAYERS` — routes foreign binaries
  automatically through Wine, Darling, Box64, or FEX-emu
- Package integration: `INSTALL <pkg>` delegates to `fusion-pkg`

Startup sequence mirrors classic DOS:
1. Parse `C:\CONFIG.SYS` (or `/etc/fusionos/config.sys`) — FILES, BUFFERS, SET
2. Run `C:\AUTOEXEC.BAT` (or `/etc/fusionos/autoexec.bat`) — PATH, PROMPT, banner

### 4.4 CONFIG.SYS and AUTOEXEC.BAT

`src/shell/config.sys` and `src/shell/autoexec.bat` are installed to
`/etc/fusionos/` on the root filesystem and to `C:\` on the FAT32 boot
partition (identical content).

`CONFIG.SYS` directives supported:
- `FILES=<n>` — maximum open file handles (informational in HAL mode)
- `BUFFERS=<n>` — disk cache buffers (informational)
- `BREAK=ON|OFF` — Ctrl+C handling
- `SHELL=<path>` — command interpreter path
- `SET key=value` — initial environment variables

`AUTOEXEC.BAT` is a standard DOS batch file.  The default sets `PATH`,
`PROMPT=$P$G`, creates `C:\TEMP`, and prints a welcome banner.

### 4.5 Package Manager — `fusion-pkg`

Source: `src/pkg/fusion_pkg.c`

Commands: `install`, `remove`, `list`, `search`, `info`, `update`

Phase 0 delegates to the HAL's package manager (`apt-get`/`pacman`/`dnf`).
Phase 1 introduces a native `.fpkg` format installed to `C:\DOS`, `C:\BIN`,
and `C:\GAMES`.

Packages will be structured as DOS-style archives:
```
C:\
  DOS\          System utilities (mirrors classic C:\DOS)
  BIN\          User utilities
  GAMES\        Game installations
  UTILS\        Developer tools
```

### 4.6 Binary Compatibility — `detect.c` + `fusion-run`

Source: `src/compat/detect.c`, `src/compat/fusion_run.c`

`fusion_detect_format()` reads magic bytes to identify ELF / PE / Mach-O.
`fusion-run` (and the `RUN` built-in) dispatches transparently:

| Binary | Launcher |
|---|---|
| ELF x86-64 (Linux native) | Direct kernel execution |
| ELF x86 | Direct (32-bit compat) |
| ELF ARM | Box86 |
| ELF ARM64 | FEX-emu |
| PE x86 (Windows) | Wine |
| PE x86-64 (Windows) | Wine64 |
| Mach-O (macOS) | Darling (experimental; Phase 3) |
| Mach-O FAT | Darling (experimental; Phase 3) |

`binfmt_misc` will be configured in Phase 1 to register PE and Mach-O magic
bytes with the kernel, enabling transparent execution without `fusion-run`.

### 4.7 GUI (Optional Layer)

The GUI is an **opt-in subsystem**, not the default environment.

- **Phase 1 (MVP)**: Weston compositor; launch from `AUTOEXEC.BAT` with
  `START DESKTOP`
- **Phase 2**: KWin (KDE Plasma) or sway (i3-Wayland) with full compositing
  and a FusionOS theme
- **In-game**: compositor bypassed via DRM direct-flip + VRR/FreeSync

Users who never want a GUI can run FusionOS as a pure DOS-like environment
without installing the GUI subsystem at all.

### 4.8 Gaming Stack

```
Game (Vulkan / DirectX call)
         │
         ├── Vulkan native  ──► Mesa (radv/ANV) ──► DRM kernel ──► GPU
         └── DirectX        ──► DXVK/VKD3D-Proton ──► Mesa ──► DRM
```

Gaming-specific kernel configuration:
- `CONFIG_PREEMPT` — kernel fully preemptible
- `CONFIG_HZ_1000` — 1 ms tick (matches classic DOS timer resolution)
- `CONFIG_HIGH_RES_TIMERS` — nanosecond sleep for game loops
- `CONFIG_JOYSTICK_XPAD` — Xbox gamepad support
- `CONFIG_HID_SONY` — PlayStation controller support
- `CONFIG_TRANSPARENT_HUGEPAGE` (madvise) — reduces TLB pressure
- `CONFIG_ZSWAP` with LZ4 — compressed RAM-based swap

---

## 5. Directory Layout (DOS view vs POSIX HAL view)

| DOS path | POSIX HAL path | Contents |
|---|---|---|
| `C:\` | `/` (FAT32 partition) | Root |
| `C:\COMMAND.COM` | `/usr/bin/fsh` | Shell binary |
| `C:\DOS` | `/etc/dos` | System utilities |
| `C:\BIN` | `/usr/local/bin` | User utilities |
| `C:\GAMES` | `/opt/games` | Game installations |
| `C:\TEMP` | `/tmp` | Temporary files |
| `C:\CONFIG.SYS` | `/etc/fusionos/config.sys` | System configuration |
| `C:\AUTOEXEC.BAT` | `/etc/fusionos/autoexec.bat` | Startup script |
| `C:\UTILS\FUSION-PKG.EXE` | `/usr/bin/fusion-pkg` | Package manager |

---

## 6. Compatibility Layer Notes

### 6.1 Linux apps
Runs natively — ELF binaries execute directly on the HAL kernel.

### 6.2 Windows apps (PE binaries)
Routed through Wine/Wine64.  `binfmt_misc` enables transparent execution
without `fusion-run`.  DXVK and VKD3D-Proton handle DirectX.

### 6.3 macOS apps (Mach-O binaries)
**Future milestone (Phase 3).**  Darling will be used for detection and
routing in Phase 0/1, but full macOS app support requires:

- Darwin syscall shim (subset of XNU syscalls)
- Objective-C runtime (libobjc)
- macOS framework stubs (Foundation, AppKit, CoreFoundation)
- Metal → Vulkan translation layer

If Darling proves insufficiently stable, a macOS VM (via QEMU/KVM +
virtio-gpu) is the Phase 3 fallback.  No partial stub will be shipped.

---

## 7. Known Trade-offs and Blockers

| Area | Trade-off |
|---|---|
| Linux HAL | User sees DOS; developer sees Linux. Dual mental model until custom kernel ships (Phase 3+). |
| FAT32 limitations | Max 4 GiB file size; no journalling. Large game files need exFAT or a secondary ext4 partition. |
| fsh is not bash | Current shell lacks pipes, job control, and scripting completeness. Phase 1 milestone: full batch + basic pipe support. |
| Darling (macOS) | Experimental; Metal not supported; macOS 13+ compatibility unverified. |
| NVIDIA driver | Requires proprietary DKMS module; Nouveau performance is insufficient for modern games. |
| Wine compatibility | ~90% Windows app compatibility; anti-cheat (EAC, BattlEye) requires kernel-level support not yet present. |
| Wayland game compat | Some engines default to X11; XWayland or gamescope required as fallback. |
| Custom kernel (future) | Replacing the Linux HAL with a native FusionDOS kernel is a multi-year Phase 3+ effort. |
