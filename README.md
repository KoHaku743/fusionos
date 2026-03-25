# FusionOS

**MS-DOS evolved into 2025.**

FusionOS is a gaming-first desktop operating system built on a **DOS-core
architecture** — it boots directly into a COMMAND.COM-like environment, uses a
FAT32 filesystem as its native storage format, and layers modern capabilities
(package management, Wayland GUI, Vulkan gaming stack, cross-platform binary
compatibility) **on top of** that DOS foundation, not in place of it.

> Think of it as what MS-DOS would have become if development had continued
> through 2025: fast, direct, fully in your control — but with GPU drivers,
> a package manager, and the ability to run Windows and Linux binaries.

---

## Architecture at a glance

```
UEFI/BIOS → GRUB2 → FusionOS Kernel (DOS-mode boot)
                            │
                    COMMAND.COM++ (fsh)
                            │
         ┌──────────────────┼──────────────────┐
         │                  │                  │
    fusion-pkg          fusion-run          (optional)
    Package mgr      Universal launcher     GUI layer
                       ELF/PE/Mach-O         Wayland
```

For the complete specification see **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.  
For the phased roadmap see **[docs/ROADMAP.md](docs/ROADMAP.md)**.

---

## Repository layout

```
src/
  shell/        COMMAND.COM++  (fsh)
    shell.c     DOS-like shell (DIR, COPY, SET, batch scripting …)
    autoexec.bat  Default startup script
    config.sys    DOS system configuration
  compat/       Binary format detection & universal launcher
    detect.c    ELF / PE32 / Mach-O magic-byte detection
    fusion_run.c  Routes binaries to Wine, Darling, Box64, FEX-emu
  pkg/          fusion-pkg — unified package manager
    fusion_pkg.c  install / remove / list / search / info / update
  fs/           FAT32 filesystem driver
    fat.h       FAT12/16/32 on-disk structures and public API
    fat.c       Read-only FAT volume driver + LFN support
    fat_info.c  fat-info CLI tool (volume inspector)
  init/         PID 1 init process + initramfs init
  monitor/      fusion-monitor — real-time system monitor
kernel/
  fusionos.config  Linux 6.6 LTS kernel config (HAL layer)
iso/boot/grub/
  grub.cfg      GRUB2 bootloader config
docs/
  ARCHITECTURE.md  Full system design document
  ROADMAP.md       Phased development plan
```

---

## Building

```sh
make          # Build all userspace components into out/bin/
make test     # Run binary-detection and shell smoke tests
make clean    # Remove build artefacts
```

Requirements: `gcc`, `make`, POSIX system headers.

---

## Quick start

```
$ ./out/bin/fsh

FusionOS COMMAND.COM Version 1.00
Type HELP for commands.  Type VER for version.

C:\>DIR
C:\>SET MYVAR=hello
C:\>ECHO %MYVAR%
hello
C:\>COMPAT out/bin/fsh
Binary   : out/bin/fsh
Format   : ELF (Linux)
Arch     : x86-64
Launcher : (native)
C:\>LAYERS
...
C:\>HELP
```

---

## Design goals

| Goal | Implementation |
|---|---|
| DOS feel | `C:\>` prompt, DIR/COPY/TYPE/SET, `.BAT` batch files |
| Modern shell | `%VAR%` expansion, aliases, history, `GOTO`/`IF`/`CALL` |
| Package mgr | `fusion-pkg install <name>` — CLI and (future) GUI |
| Gaming | PREEMPT kernel, HZ=1000, DXVK, gamepad drivers |
| Cross-platform | `fusion-run` auto-routes PE→Wine, Mach-O→Darling |
| FAT32 native | `fat.c` driver reads FAT12/16/32 volumes directly |
