#!/bin/bash
# FusionOS ISO Build Script
# Builds the kernel, userspace components, initramfs, and final ISO image.
set -e

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
ISO_DIR="${SCRIPT_DIR}/iso"
SRC_DIR="${SCRIPT_DIR}/src"
KERNEL_DIR="${SCRIPT_DIR}/kernel"

KERNEL_VERSION="6.6"
KERNEL_SRC_DIR="${BUILD_DIR}/linux-${KERNEL_VERSION}"
KERNEL_TARBALL="${BUILD_DIR}/linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz"

BUSYBOX_VERSION="1.36.1"
BUSYBOX_SRC_DIR="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}"
BUSYBOX_TARBALL="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"

INITRAMFS_DIR="${BUILD_DIR}/initramfs"
ISO_OUTPUT="${BUILD_DIR}/fusionos.iso"

JOBS="${JOBS:-$(nproc)}"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
info()  { echo "[INFO]  $*"; }
warn()  { echo "[WARN]  $*" >&2; }
error() { echo "[ERROR] $*" >&2; exit 1; }
step()  { echo; echo "==> $*"; }

# ---------------------------------------------------------------------------
# Prerequisite check
# ---------------------------------------------------------------------------
check_deps() {
    step "Checking build dependencies"
    local missing=()
    for dep in gcc make tar xz bzip2 cpio gzip grub-mkrescue xorriso; do
        if ! command -v "$dep" &>/dev/null; then
            missing+=("$dep")
        fi
    done
    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}.  Install them before building."
    fi
    info "All dependencies found."
}

# ---------------------------------------------------------------------------
# Directory setup
# ---------------------------------------------------------------------------
setup_dirs() {
    step "Setting up build directories"
    mkdir -p "${BUILD_DIR}"
    mkdir -p "${INITRAMFS_DIR}"/{bin,sbin,usr/bin,usr/sbin,lib,lib64,proc,sys,dev,tmp,newroot,etc,var/lib/fusion-pkg,var/cache/fusion-pkg}
    info "Build directories ready."
}

# ---------------------------------------------------------------------------
# Kernel build
# ---------------------------------------------------------------------------
build_kernel() {
    step "Building Linux ${KERNEL_VERSION} kernel"

    if [ ! -f "${KERNEL_TARBALL}" ]; then
        info "Downloading kernel source..."
        wget -q -O "${KERNEL_TARBALL}" "${KERNEL_URL}" \
            || error "Failed to download kernel source."
    fi

    if [ ! -d "${KERNEL_SRC_DIR}" ]; then
        info "Extracting kernel source..."
        tar -xf "${KERNEL_TARBALL}" -C "${BUILD_DIR}"
    fi

    info "Copying FusionOS kernel configuration..."
    cp "${KERNEL_DIR}/fusionos.config" "${KERNEL_SRC_DIR}/.config"

    info "Building kernel (this may take a while)..."
    make -C "${KERNEL_SRC_DIR}" olddefconfig
    make -C "${KERNEL_SRC_DIR}" -j"${JOBS}" bzImage

    local vmlinuz="${KERNEL_SRC_DIR}/arch/x86/boot/bzImage"
    [ -f "${vmlinuz}" ] || error "Kernel build failed: bzImage not found."
    cp "${vmlinuz}" "${ISO_DIR}/boot/vmlinuz"
    info "Kernel built and placed at iso/boot/vmlinuz."
}

# ---------------------------------------------------------------------------
# BusyBox build (provides sh, mdev, etc. for initramfs)
# ---------------------------------------------------------------------------
build_busybox() {
    step "Building BusyBox ${BUSYBOX_VERSION}"

    if [ ! -f "${BUSYBOX_TARBALL}" ]; then
        info "Downloading BusyBox..."
        wget -q -O "${BUSYBOX_TARBALL}" "${BUSYBOX_URL}" \
            || error "Failed to download BusyBox."
    fi

    if [ ! -d "${BUSYBOX_SRC_DIR}" ]; then
        info "Extracting BusyBox..."
        tar -xf "${BUSYBOX_TARBALL}" -C "${BUILD_DIR}"
    fi

    info "Configuring BusyBox (static build)..."
    make -C "${BUSYBOX_SRC_DIR}" defconfig
    # Force static linking so the initramfs needs no shared libraries
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
        "${BUSYBOX_SRC_DIR}/.config" 2>/dev/null || true
    echo "CONFIG_STATIC=y" >> "${BUSYBOX_SRC_DIR}/.config"

    info "Building BusyBox..."
    make -C "${BUSYBOX_SRC_DIR}" -j"${JOBS}"

    info "Installing BusyBox into initramfs..."
    make -C "${BUSYBOX_SRC_DIR}" CONFIG_PREFIX="${INITRAMFS_DIR}" install
}

# ---------------------------------------------------------------------------
# Userspace component build
# ---------------------------------------------------------------------------
build_userspace() {
    step "Building FusionOS userspace components"

    local CC="${CC:-gcc}"
    local CFLAGS="${CFLAGS:--O2 -Wall -Wextra -static}"

    # initramfs init (tiny, no-libc-dependency style)
    info "Building initramfs init..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/init" \
        "${SRC_DIR}/init/initramfs_init.c"

    # main init (PID 1 after switch_root)
    info "Building main init..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/sbin/init" \
        "${SRC_DIR}/init/init.c"

    # fsh — FusionOS shell
    info "Building fsh shell..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/bin/fsh" \
        "${SRC_DIR}/shell/shell.c" \
        "${SRC_DIR}/compat/detect.c"

    # fusion-run — universal binary launcher
    info "Building fusion-run..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/usr/bin/fusion-run" \
        "${SRC_DIR}/compat/fusion_run.c" \
        "${SRC_DIR}/compat/detect.c"

    # fusion-pkg — package manager
    info "Building fusion-pkg..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/usr/bin/fusion-pkg" \
        "${SRC_DIR}/pkg/fusion_pkg.c"

    # fusion-monitor — system monitor
    info "Building fusion-monitor..."
    "$CC" $CFLAGS -o "${INITRAMFS_DIR}/usr/bin/fusion-monitor" \
        "${SRC_DIR}/monitor/monitor.c"

    # detect_test helper
    info "Building detect_test utility..."
    "$CC" $CFLAGS -o "${BUILD_DIR}/detect_test" \
        "${SRC_DIR}/compat/detect_test.c" \
        "${SRC_DIR}/compat/detect.c"

    chmod +x "${INITRAMFS_DIR}/init"
    info "Userspace components built."
}

# ---------------------------------------------------------------------------
# /etc population
# ---------------------------------------------------------------------------
populate_etc() {
    step "Populating /etc inside initramfs"

    cat > "${INITRAMFS_DIR}/etc/passwd" << 'EOF'
root:x:0:0:root:/root:/bin/fsh
EOF

    cat > "${INITRAMFS_DIR}/etc/group" << 'EOF'
root:x:0:
EOF

    cat > "${INITRAMFS_DIR}/etc/hostname" << 'EOF'
fusionos
EOF

    cat > "${INITRAMFS_DIR}/etc/os-release" << 'EOF'
NAME="FusionOS"
VERSION="1.0"
ID=fusionos
PRETTY_NAME="FusionOS 1.0"
HOME_URL="https://github.com/KoHaku743/fusionos"
EOF

    info "/etc populated."
}

# ---------------------------------------------------------------------------
# Initramfs (cpio) creation
# ---------------------------------------------------------------------------
build_initramfs() {
    step "Creating initramfs (cpio.gz)"

    local initrd="${ISO_DIR}/boot/initrd"

    cd "${INITRAMFS_DIR}"
    find . | cpio -o -H newc 2>/dev/null | gzip -9 > "${initrd}"
    cd "${SCRIPT_DIR}"

    local size
    size=$(du -sh "${initrd}" | cut -f1)
    info "Initramfs created at iso/boot/initrd (${size})."
}

# ---------------------------------------------------------------------------
# ISO assembly
# ---------------------------------------------------------------------------
build_iso() {
    step "Assembling ISO image with GRUB2"

    # Ensure GRUB fonts directory exists
    mkdir -p "${ISO_DIR}/boot/grub/fonts"

    # Copy the unicode font if available (optional, graceful fallback in grub.cfg)
    local unicode_font
    unicode_font=$(find /usr/share/grub /usr/share/grub2 -name "unicode.pf2" 2>/dev/null | head -1)
    if [ -n "${unicode_font}" ]; then
        cp "${unicode_font}" "${ISO_DIR}/boot/grub/fonts/"
        info "Copied GRUB unicode font."
    else
        warn "GRUB unicode font not found; graphical boot menu may be limited."
    fi

    info "Running grub-mkrescue..."
    grub-mkrescue -o "${ISO_OUTPUT}" "${ISO_DIR}" \
        -- -volid FUSIONOS 2>/dev/null \
        || error "grub-mkrescue failed."

    local size
    size=$(du -sh "${ISO_OUTPUT}" | cut -f1)
    info "ISO image created: ${ISO_OUTPUT} (${size})"
}

# ---------------------------------------------------------------------------
# Quick smoke-test (QEMU, optional)
# ---------------------------------------------------------------------------
test_iso() {
    step "Smoke-testing ISO with QEMU (optional)"

    if ! command -v qemu-system-x86_64 &>/dev/null; then
        warn "qemu-system-x86_64 not found; skipping ISO smoke-test."
        return 0
    fi

    info "Booting ISO in QEMU (serial output, 30 s timeout)..."
    timeout 30 qemu-system-x86_64 \
        -m 256M \
        -cdrom "${ISO_OUTPUT}" \
        -nographic \
        -serial stdio \
        -no-reboot \
        -display none \
        2>&1 | head -40 || true

    info "QEMU test complete."
}

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
clean() {
    step "Cleaning build directory"
    rm -rf "${BUILD_DIR}"
    rm -f "${ISO_DIR}/boot/vmlinuz" "${ISO_DIR}/boot/initrd"
    info "Clean complete."
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat << 'EOF'
FusionOS ISO Build Script

Usage: build.sh [target...]

Targets:
  all          Full build: deps → setup → kernel → busybox →
                           userspace → etc → initramfs → iso (default)
  kernel       Build the Linux kernel only
  busybox      Build BusyBox only
  userspace    Build FusionOS userspace components only
  initramfs    Create the initramfs cpio image only
  iso          Assemble the final ISO only
  test         Boot the ISO in QEMU for a quick smoke-test
  clean        Remove all build artefacts

Environment variables:
  JOBS=N       Parallel build jobs (default: nproc)
  CC           C compiler (default: gcc)
  CFLAGS       Compiler flags (default: -O2 -Wall -Wextra -static)
EOF
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
TARGET="${1:-all}"

case "${TARGET}" in
    all)
        check_deps
        setup_dirs
        build_kernel
        build_busybox
        build_userspace
        populate_etc
        build_initramfs
        build_iso
        echo
        info "Build complete!  ISO: ${ISO_OUTPUT}"
        ;;
    kernel)    check_deps; setup_dirs; build_kernel    ;;
    busybox)   check_deps; setup_dirs; build_busybox   ;;
    userspace) check_deps; setup_dirs; build_userspace ;;
    initramfs) build_initramfs ;;
    iso)       build_iso ;;
    test)      test_iso ;;
    clean)     clean ;;
    help|-h|--help) usage ;;
    *)
        error "Unknown target '${TARGET}'.  Run 'build.sh help' for usage."
        ;;
esac
