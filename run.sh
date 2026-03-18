#!/bin/bash
# FusionOS QEMU launcher
# Boots the FusionOS ISO with KVM acceleration, virtio-net, port forwarding,
# and audio.  Falls back gracefully when KVM or audio is unavailable.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO="${SCRIPT_DIR}/build/fusionos.iso"

# ---------------------------------------------------------------------------
# Defaults (overridable via environment)
RAM="${RAM:-512M}"
CPUS="${CPUS:-2}"
SSH_HOST_PORT="${SSH_HOST_PORT:-2222}"
HTTP_HOST_PORT="${HTTP_HOST_PORT:-8080}"
VNC_DISPLAY="${VNC_DISPLAY:-}"     # empty = no VNC

# ---------------------------------------------------------------------------
info()  { echo "[run]  $*"; }
warn()  { echo "[warn] $*" >&2; }
die()   { echo "[err]  $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Check that the ISO exists
[ -f "${ISO}" ] || die "ISO not found: ${ISO}.  Run ./build.sh first."

# ---------------------------------------------------------------------------
# Detect KVM availability
KVM_ARGS=()
if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    KVM_ARGS=("-enable-kvm" "-cpu" "host")
    info "KVM acceleration enabled."
else
    warn "KVM not available; running in software emulation (slow)."
    KVM_ARGS=("-cpu" "qemu64")
fi

# ---------------------------------------------------------------------------
# Audio backend detection
AUDIO_ARGS=()
if command -v pulseaudio &>/dev/null && pulseaudio --check 2>/dev/null; then
    AUDIO_ARGS=("-audiodev" "pa,id=audio0" "-device" "intel-hda"
                "-device" "hda-duplex,audiodev=audio0")
    info "PulseAudio detected; enabling audio."
elif command -v pipewire &>/dev/null; then
    AUDIO_ARGS=("-audiodev" "pipewire,id=audio0" "-device" "intel-hda"
                "-device" "hda-duplex,audiodev=audio0")
    info "PipeWire detected; enabling audio."
else
    warn "No supported audio backend found; running without audio."
fi

# ---------------------------------------------------------------------------
# Optional VNC display
DISPLAY_ARGS=()
if [ -n "${VNC_DISPLAY}" ]; then
    DISPLAY_ARGS=("-vnc" "${VNC_DISPLAY}")
    info "VNC display: ${VNC_DISPLAY}"
else
    DISPLAY_ARGS=("-display" "sdl" "-vga" "virtio")
fi

# ---------------------------------------------------------------------------
# Network: virtio-net with port forwarding for SSH and HTTP
NETDEV="user,id=net0,hostfwd=tcp::${SSH_HOST_PORT}-:22,hostfwd=tcp::${HTTP_HOST_PORT}-:80"

info "Starting FusionOS in QEMU..."
info "  ISO     : ${ISO}"
info "  RAM     : ${RAM}"
info "  CPUs    : ${CPUS}"
info "  SSH     : localhost:${SSH_HOST_PORT} → guest:22"
info "  HTTP    : localhost:${HTTP_HOST_PORT} → guest:80"

exec qemu-system-x86_64 \
    "${KVM_ARGS[@]}" \
    -m "${RAM}" \
    -smp "${CPUS}" \
    -cdrom "${ISO}" \
    -boot d \
    -netdev "${NETDEV}" \
    -device virtio-net-pci,netdev=net0 \
    "${AUDIO_ARGS[@]}" \
    "${DISPLAY_ARGS[@]}" \
    -serial mon:stdio \
    -no-reboot \
    "$@"
