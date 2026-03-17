#!/bin/bash
# run.sh — launch FusionOS ISO in QEMU (x86_64)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO="${SCRIPT_DIR}/build/fusionos.iso"

if [ ! -f "$ISO" ]; then
    echo "ISO not found: $ISO"
    echo "Run ./build.sh first to create the ISO."
    exit 1
fi

# Detect KVM availability
KVM_FLAGS=()
if [ -e /dev/kvm ]; then
    KVM_FLAGS=(-enable-kvm -cpu host)
else
    KVM_FLAGS=(-cpu qemu64)
    echo "Warning: KVM not available, running without hardware acceleration"
fi

# Detect display backend
DISPLAY_FLAGS=(-display sdl)
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_FLAGS=(-nographic)
fi

echo "Launching FusionOS ISO: $ISO"
echo "QEMU: press Ctrl-A x (serial) or Ctrl-C to quit"

exec qemu-system-x86_64 \
    "${KVM_FLAGS[@]}" \
    -m 2G \
    -smp "$(nproc)" \
    -machine q35 \
    -drive "file=${ISO},format=raw,media=cdrom,readonly=on" \
    -boot d \
    -netdev "user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80" \
    -device virtio-net-pci,netdev=net0 \
    -device virtio-balloon-pci \
    -serial stdio \
    "${DISPLAY_FLAGS[@]}" \
    "$@"
