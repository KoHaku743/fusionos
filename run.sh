#!/bin/bash
# run.sh — Launch FusionOS in QEMU (x86_64)
# Requires: qemu-system-x86_64, KVM (optional), the built ISO
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO="${SCRIPT_DIR}/build/fusionos.iso"

if [ ! -f "$ISO" ]; then
    echo "ERROR: ISO not found at $ISO" >&2
    echo "Run './build.sh' first to build the ISO." >&2
    exit 1
fi

# Detect KVM availability
KVM_OPTS=""
if [ -w /dev/kvm ]; then
    KVM_OPTS="-enable-kvm -cpu host"
    echo "[*] KVM acceleration enabled"
else
    KVM_OPTS="-cpu qemu64"
    echo "[*] KVM not available, running without acceleration"
fi

echo "[*] Starting FusionOS in QEMU..."
echo "[*] ISO: $ISO"
echo "[*] Serial output: stdio (press Ctrl+A X to quit QEMU)"
echo

DISK="${SCRIPT_DIR}/build/fusionos-disk.qcow2"
if [ ! -f "$DISK" ]; then
    echo "[*] Creating persistent disk image: $DISK (4GB)"
    qemu-img create -f qcow2 "$DISK" 4G
fi

exec qemu-system-x86_64 \
    $KVM_OPTS \
    -m 2048 \
    -smp 2 \
    -cdrom "$ISO" \
    -boot d \
    -nographic \
    -serial mon:stdio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80 \
    -device virtio-blk-pci,drive=hd0 \
    -drive file="$DISK",format=qcow2,if=none,id=hd0,media=disk \
    -audiodev pa,id=snd0 2>/dev/null \
    -device virtio-sound-pci,audiodev=snd0 2>/dev/null \
    "$@" || \
exec qemu-system-x86_64 \
    $KVM_OPTS \
    -m 2048 \
    -smp 2 \
    -cdrom "$ISO" \
    -boot d \
    -nographic \
    -serial mon:stdio \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::8080-:80 \
    "$@"
