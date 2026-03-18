#!/bin/bash
# FusionOS ISO Build Script
# Builds the kernel, userspace components, initramfs, and final ISO image.
set -euo pipefail

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
# Cleanup trap — called on any EXIT (success or failure)
# ---------------------------------------------------------------------------
cleanup() {
    local rc=$?
    if [ "${rc}" -ne 0 ]; then
        warn "Build failed (exit code ${rc})."
        warn "Partial artefacts may remain in: ${BUILD_DIR}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Prerequisite check
# ---------------------------------------------------------------------------
check_deps() {
    step "Checking build dependencies"
    local missing=()
    for dep in gcc make tar xz bzip2 cpio gzip grub-mkrescue xorriso strip; do
        if ! command -v "$dep" &>/dev/null; then
            missing+=("$dep")
        fi
    done
    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}.  Install them before building."
    fi
    info "All dependencies found."
}

# Lighter dep check — only tools needed for compiling userspace
check_deps_userspace() {
    step "Checking userspace build dependencies"
    local missing=()
    for dep in gcc make strip; do
        if ! command -v "$dep" &>/dev/null; then
            missing+=("$dep")
        fi
    done
    if [ ${#missing[@]} -ne 0 ]; then
        error "Missing dependencies: ${missing[*]}."
    fi
    info "Userspace dependencies found."
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
    cp "${SCRIPT_DIR}/kernel/fusionos.config" "${KERNEL_SRC_DIR}/.config"

    info "Building kernel (this may take a while)..."
    make -C "${KERNEL_SRC_DIR}" olddefconfig
    make -C "${KERNEL_SRC_DIR}" -j"${JOBS}" bzImage

    info "Installing kernel modules..."
    make -C "${KERNEL_SRC_DIR}" \
        INSTALL_MOD_PATH="${INITRAMFS_DIR}" \
        modules_install

    info "Stripping debug symbols from kernel modules..."
    find "${INITRAMFS_DIR}/lib/modules" -name "*.ko" \
        -exec strip --strip-debug {} \; 2>/dev/null || true

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

    info "Configuring BusyBox..."
    # Start from the default config
    make -C "${BUSYBOX_SRC_DIR}" defconfig

    # Force static linking (initramfs has no shared libs)
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
        "${BUSYBOX_SRC_DIR}/.config" 2>/dev/null || true
    echo "CONFIG_STATIC=y" >> "${BUSYBOX_SRC_DIR}/.config"

    # Disable the BusyBox init to avoid conflicts with our custom PID-1 init
    sed -i 's/^CONFIG_INIT=y/# CONFIG_INIT is not set/' \
        "${BUSYBOX_SRC_DIR}/.config" 2>/dev/null || true
    echo "# CONFIG_INIT is not set" >> "${BUSYBOX_SRC_DIR}/.config"
    echo "# CONFIG_FEATURE_INIT_SYSLOG is not set" >> "${BUSYBOX_SRC_DIR}/.config"
    echo "# CONFIG_FEATURE_INIT_COREDUMPS is not set" >> "${BUSYBOX_SRC_DIR}/.config"

    # Re-run oldconfig to resolve any dependency conflicts without interaction
    yes "" | make -C "${BUSYBOX_SRC_DIR}" oldconfig

    info "Building BusyBox..."
    make -C "${BUSYBOX_SRC_DIR}" -j"${JOBS}"

    info "Installing BusyBox into initramfs..."
    make -C "${BUSYBOX_SRC_DIR}" CONFIG_PREFIX="${INITRAMFS_DIR}" install
}

# ---------------------------------------------------------------------------
# Shared libraries (minimal set for non-static binaries at runtime)
# ---------------------------------------------------------------------------
build_libs() {
    step "Copying minimal shared libraries into sysroot"

    local lib_src=""
    for d in /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib64; do
        if [ -d "$d" ]; then
            lib_src="$d"
            break
        fi
    done

    if [ -z "${lib_src}" ]; then
        warn "Could not locate host libc directory; skipping shared lib copy."
        return 0
    fi

    local dest="${INITRAMFS_DIR}/lib"
    mkdir -p "${dest}"

    # Copy the essential runtime libraries
    for lib in \
        libc.so.6 \
        libm.so.6 \
        libdl.so.2 \
        libpthread.so.0 \
        librt.so.1 \
        ld-linux-x86-64.so.2; do
        if [ -f "${lib_src}/${lib}" ]; then
            cp -v "${lib_src}/${lib}" "${dest}/" || true
        elif [ -L "${lib_src}/${lib}" ]; then
            cp -Pv "${lib_src}/${lib}" "${dest}/" || true
        fi
    done

    # Ensure /lib64 symlink for the dynamic linker
    if [ ! -e "${INITRAMFS_DIR}/lib64" ]; then
        ln -s lib "${INITRAMFS_DIR}/lib64"
    fi

    info "Shared libraries copied."
}

# ---------------------------------------------------------------------------
# Userspace component build
# ---------------------------------------------------------------------------
build_userspace() {
    step "Building FusionOS userspace components"

    local CC="${CC:-gcc}"
    local CFLAGS="${CFLAGS:--O2 -Wall -Wextra -Wpedantic -std=c11 -static}"

    # initramfs init (tiny, syscall-level; no printf)
    info "Building initramfs init..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/init" \
        "${SRC_DIR}/init/initramfs_init.c"

    # main init (PID 1 after switch_root)
    info "Building main init..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/sbin/init" \
        "${SRC_DIR}/init/init.c"

    # fsh — FusionOS shell
    info "Building fsh shell..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/bin/fsh" \
        "${SRC_DIR}/shell/shell.c" \
        "${SRC_DIR}/compat/detect.c"

    # fusion-run — universal binary launcher
    info "Building fusion-run..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/usr/bin/fusion-run" \
        "${SRC_DIR}/compat/fusion_run.c" \
        "${SRC_DIR}/compat/detect.c"

    # fusion-pkg — package manager
    info "Building fusion-pkg..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/usr/bin/fusion-pkg" \
        "${SRC_DIR}/pkg/fusion_pkg.c"

    # fusion-monitor — system monitor TUI
    info "Building fusion-monitor..."
    "$CC" ${CFLAGS} -o "${INITRAMFS_DIR}/usr/bin/fusion-monitor" \
        "${SRC_DIR}/monitor/monitor.c"

    # detect_test — unit-test helper (placed in build/, not initramfs)
    info "Building detect_test utility..."
    "$CC" ${CFLAGS} -o "${BUILD_DIR}/detect_test" \
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
# Initramfs (cpio.gz) creation
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
# ISO assembly — BIOS + EFI, xz-compressed
# ---------------------------------------------------------------------------
build_iso() {
    step "Assembling ISO image with GRUB2 (BIOS + EFI)"

    # Ensure GRUB fonts directory exists
    mkdir -p "${ISO_DIR}/boot/grub/fonts"

    # Copy the unicode font if available (graceful fallback in grub.cfg)
    local unicode_font
    unicode_font=$(find /usr/share/grub /usr/share/grub2 \
                        -name "unicode.pf2" 2>/dev/null | head -1 || true)
    if [ -n "${unicode_font}" ]; then
        cp "${unicode_font}" "${ISO_DIR}/boot/grub/fonts/"
        info "Copied GRUB unicode font."
    else
        warn "GRUB unicode font not found; graphical boot menu may be limited."
    fi

    info "Running grub-mkrescue (BIOS + EFI, xz compression)..."
    grub-mkrescue \
        --compress=xz \
        --product-name="FusionOS" \
        -o "${ISO_OUTPUT}" \
        "${ISO_DIR}" \
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
# Build only the FusionOS userspace (no kernel/BusyBox downloads)
# ---------------------------------------------------------------------------
build_rootfs() {
    check_deps_userspace
    setup_dirs
    build_userspace
    populate_etc
    info "Rootfs userspace built in ${INITRAMFS_DIR}."
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
usage() {
    cat << 'EOF'
FusionOS ISO Build Script

Usage: build.sh [target...]

Targets:
  all          Full build: deps → setup → kernel → busybox → libs →
                           userspace → etc → initramfs → iso  (default)
  rootfs       Build only the FusionOS userspace components
  kernel       Build the Linux kernel only
  busybox      Build BusyBox only
  libs         Copy shared libraries into sysroot
  userspace    Build FusionOS userspace components only
  initramfs    Create the initramfs cpio image only
  iso          Assemble the final ISO only
  test         Boot the ISO in QEMU for a quick smoke-test
  clean        Remove all build artefacts

Environment variables:
  JOBS=N       Parallel build jobs (default: nproc)
  CC           C compiler (default: gcc)
  CFLAGS       Compiler flags (default: -O2 -Wall -Wextra -Wpedantic -std=c11 -static)
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
        build_libs
        build_userspace
        populate_etc
        build_initramfs
        build_iso
        echo
        info "Build complete!  ISO: ${ISO_OUTPUT}"
        ;;
    rootfs)    build_rootfs   ;;
    kernel)    check_deps; setup_dirs; build_kernel    ;;
    busybox)   check_deps; setup_dirs; build_busybox   ;;
    libs)      check_deps; setup_dirs; build_libs      ;;
    userspace) check_deps_userspace; setup_dirs; build_userspace ;;
    initramfs) build_initramfs ;;
    iso)       build_iso ;;
    test)      test_iso ;;
    clean)     clean ;;
    help|-h|--help) usage ;;
    *)
        error "Unknown target '${TARGET}'.  Run 'build.sh help' for usage."
        ;;
esac
