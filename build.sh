#!/bin/bash
set -euo pipefail

# FusionOS Build Script - Hardenedbootable ISO pipeline
# Kernel 6.6 LTS + custom compat layer init + BusyBox + GRUB2

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
SYSROOT="${BUILD_DIR}/sysroot"
INITRAMFS="${BUILD_DIR}/initramfs"
ISO_DIR="${BUILD_DIR}/iso"
KERNEL_VERSION="6.6.13"
BUSYBOX_VERSION="1.36.1"
BUSYBOX_TAR="busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/${BUSYBOX_TAR}"

# Cleanup function
cleanup() {
    local exit_code=$?
    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}[ERROR]${NC} Build failed at exit code $exit_code"
        echo "Preserving build artifacts in: $BUILD_DIR"
    fi
    return $exit_code
}
trap cleanup EXIT

log_info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

# Create basic directory structure
init_build_dirs() {
    log_info "Initializing build directories"
    mkdir -p "$BUILD_DIR" "$SYSROOT" "$INITRAMFS" "$ISO_DIR"
    mkdir -p "${SYSROOT}/bin" "${SYSROOT}/sbin" "${SYSROOT}/lib" "${SYSROOT}/lib64"
    mkdir -p "${SYSROOT}/usr/bin" "${SYSROOT}/usr/sbin" "${SYSROOT}/usr/lib"
    mkdir -p "${SYSROOT}/proc" "${SYSROOT}/sys" "${SYSROOT}/dev" "${SYSROOT}/tmp" "${SYSROOT}/root"
    mkdir -p "${SYSROOT}/var/lib/fusion-pkg" "${SYSROOT}/var/log" "${SYSROOT}/var/run"
    mkdir -p "${SYSROOT}/etc"
    chmod 1777 "${SYSROOT}/tmp"
}

# Build custom binaries
build_binaries() {
    log_info "Compiling custom binaries"
    
    # Detect library (needed by others)
    gcc -c -O2 -Wall -Wextra -Wpedantic -std=c11 \
        "${SCRIPT_DIR}/src/compat/detect.c" \
        -o "${BUILD_DIR}/detect.o"
    
    # init (PID 1)
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/init/init.c" \
        -o "${SYSROOT}/sbin/init"
    strip --strip-all "${SYSROOT}/sbin/init"
    
    # initramfs_init (first process, must stay under 100KB)
    # Prefer musl-gcc for a tiny static binary; fall back to gcc
    if command -v musl-gcc >/dev/null 2>&1; then
        musl-gcc -Os -static \
            "${SCRIPT_DIR}/src/init/initramfs_init.c" \
            -o "${BUILD_DIR}/initramfs_init"
    else
        gcc -Os -static \
            "${SCRIPT_DIR}/src/init/initramfs_init.c" \
            -o "${BUILD_DIR}/initramfs_init"
    fi
    strip --strip-all "${BUILD_DIR}/initramfs_init"
    
    # shell (fsh)
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/compat/detect.c" \
        "${SCRIPT_DIR}/src/shell/shell.c" \
        -o "${SYSROOT}/bin/fsh"
    strip --strip-all "${SYSROOT}/bin/fsh"
    
    # fusion-run launcher
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/compat/detect.c" \
        "${SCRIPT_DIR}/src/compat/fusion_run.c" \
        -o "${SYSROOT}/usr/bin/fusion-run"
    strip --strip-all "${SYSROOT}/usr/bin/fusion-run"
    
    # Symbolic link for execution
    ln -sf /usr/bin/fusion-run "${SYSROOT}/bin/fusion-run" 2>/dev/null || true
    
    # fusion-pkg package manager
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/pkg/fusion_pkg.c" \
        -o "${SYSROOT}/usr/bin/fusion-pkg"
    strip --strip-all "${SYSROOT}/usr/bin/fusion-pkg"
    
    # fusion-monitor system monitor
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/monitor/monitor.c" \
        -o "${SYSROOT}/usr/bin/fusion-monitor"
    strip --strip-all "${SYSROOT}/usr/bin/fusion-monitor"
    
    # detect_test
    gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -static \
        "${SCRIPT_DIR}/src/compat/detect.c" \
        "${SCRIPT_DIR}/src/compat/detect_test.c" \
        -o "${BUILD_DIR}/detect_test"
    strip --strip-all "${BUILD_DIR}/detect_test"
}

# Setup binfmt_misc registration
setup_binfmt() {
    log_info "Creating binfmt_misc setup script"
    cat > "${SYSROOT}/etc/init.d/binfmt" << 'EOF'
#!/bin/sh
# Register PE (Windows) and Mach-O (macOS) binary formats

if [ ! -d /proc/sys/fs/binfmt_misc ]; then
    mount -t binfmt_misc none /proc/sys/fs/binfmt_misc || exit 1
fi

# PE32/PE64 (Windows binaries)
echo ':wine:M::MZ::/usr/bin/fusion-run:' > /proc/sys/fs/binfmt_misc/register 2>/dev/null || true

# Mach-O 32-bit
echo ':macho32:M::cafebabe:ffffffffff::/usr/bin/fusion-run:' > /proc/sys/fs/binfmt_misc/register 2>/dev/null || true

# Mach-O 64-bit
echo ':macho64:M::feedfacf:ffffffff::/usr/bin/fusion-run:' > /proc/sys/fs/binfmt_misc/register 2>/dev/null || true

# Mach-O FAT Universal
echo ':machofat:M::cafebabf:ffffffff::/usr/bin/fusion-run:' > /proc/sys/fs/binfmt_misc/register 2>/dev/null || true

exit 0
EOF
    chmod +x "${SYSROOT}/etc/init.d/binfmt"
}

# Setup /etc/fstab
setup_fstab() {
    log_info "Creating /etc/fstab"
    cat > "${SYSROOT}/etc/fstab" << 'EOF'
# FusionOS fstab
proc        /proc     proc    defaults            0 0
sysfs       /sys      sysfs   defaults            0 0
devtmpfs    /dev      devtmpfs mode=755           0 0
tmpfs       /tmp      tmpfs   mode=1777           0 0
tmpfs       /var/run  tmpfs   mode=755            0 0
EOF
}

# Setup minimal /etc/passwd and /etc/group
setup_users() {
    log_info "Creating user database"
    cat > "${SYSROOT}/etc/passwd" << 'EOF'
root:x:0:0:root:/root:/bin/fsh
EOF
    cat > "${SYSROOT}/etc/group" << 'EOF'
root:x:0:
EOF
    chmod 644 "${SYSROOT}/etc/passwd" "${SYSROOT}/etc/group"
}

# Build kernel
build_kernel() {
    log_info "Downloading Linux kernel ${KERNEL_VERSION}"
    local kernel_tar="linux-${KERNEL_VERSION}.tar.xz"
    local kernel_url="https://cdn.kernel.org/pub/linux/kernel/v6.x/${kernel_tar}"
    
    if [ ! -f "${BUILD_DIR}/${kernel_tar}" ]; then
        cd "${BUILD_DIR}"
        wget -q --show-progress "$kernel_url"
        tar -xf "$kernel_tar"
        cd -
    fi
    
    log_info "Building kernel"
    local kernel_src="${BUILD_DIR}/linux-${KERNEL_VERSION}"
    
    # Copy config using absolute path
    cp "${SCRIPT_DIR}/kernel/fusionos.config" "${kernel_src}/.config"
    
    cd "${kernel_src}"
    make -j"$(nproc)" oldconfig
    make -j"$(nproc)" bzImage
    make modules_install INSTALL_MOD_PATH="${SYSROOT}"
    
    # Strip debug symbols from modules
    find "${SYSROOT}/lib/modules" -name "*.ko" -exec strip --strip-unneeded {} \;
    
    cp arch/x86/boot/bzImage "${BUILD_DIR}/kernel"
    cd -
}

# Build BusyBox
build_busybox() {
    log_info "Downloading BusyBox ${BUSYBOX_VERSION}"
    
    if [ ! -f "${BUILD_DIR}/${BUSYBOX_TAR}" ]; then
        cd "${BUILD_DIR}"
        wget -q --show-progress "$BUSYBOX_URL"
        cd -
    fi
    
    if [ ! -d "${BUILD_DIR}/busybox-${BUSYBOX_VERSION}" ]; then
        cd "${BUILD_DIR}"
        tar -xf "$BUSYBOX_TAR"
        cd -
    fi
    
    log_info "Building BusyBox"
    local busybox_src="${BUILD_DIR}/busybox-${BUSYBOX_VERSION}"
    cd "${busybox_src}"
    
    # Use defconfig as base
    make defconfig
    
    # Disable conflicting features
    sed -i 's/^CONFIG_INIT=y/# CONFIG_INIT is not set/' .config
    sed -i 's/^CONFIG_FEATURE_INIT_SYSLOG=y/# CONFIG_FEATURE_INIT_SYSLOG is not set/' .config
    
    # Enable static linking
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
    
    make -j"$(nproc)" LDFLAGS="-static"
    make install CONFIG_PREFIX="${SYSROOT}"
    
    cd -
}

# Copy minimal shared libraries for non-static binaries
build_libs() {
    log_info "Copying minimal shared libraries"
    
    # Find and copy libc, libm, libdl - needed by some tools
    local libs=("libc.so.6" "libm.so.6" "libdl.so.2" "libpthread.so.0")
    
    for lib in "${libs[@]}"; do
        if [ -f "/lib64/$lib" ]; then
            cp "/lib64/$lib" "${SYSROOT}/lib64/" 2>/dev/null || true
        fi
        if [ -f "/lib/x86_64-linux-gnu/$lib" ]; then
            cp "/lib/x86_64-linux-gnu/$lib" "${SYSROOT}/lib64/" 2>/dev/null || true
        fi
        if [ -f "/lib/$lib" ]; then
            cp "/lib/$lib" "${SYSROOT}/lib/" 2>/dev/null || true
        fi
    done
    
    # Copy ld-linux
    if [ -f "/lib64/ld-linux-x86-64.so.2" ]; then
        cp "/lib64/ld-linux-x86-64.so.2" "${SYSROOT}/lib64/"
    fi
}

# Build initramfs with initramfs_init as init
build_initramfs() {
    log_info "Building initramfs"
    
    mkdir -p "${INITRAMFS}"/{bin,sbin,lib,proc,sys,dev,newroot}
    
    # Copy minimal binaries needed in initramfs
    cp "${BUILD_DIR}/initramfs_init" "${INITRAMFS}/init"
    
    # Copy essential BusyBox tools
    cp "${SYSROOT}/bin/busybox" "${INITRAMFS}/bin/"
    cd "${INITRAMFS}/bin"
    for cmd in sh cat grep ls mdev mount umount; do
        ln -sf busybox "$cmd" 2>/dev/null || true
    done
    cd -
    
    # Create cpio archive
    cd "${INITRAMFS}"
    find . -print0 | cpio -0o -H newc -R 0:0 | gzip > "${BUILD_DIR}/initramfs.cpio.gz"
    cd -
}

# Build ISO
build_iso() {
    log_info "Building ISO with GRUB2"
    
    mkdir -p "${ISO_DIR}/boot/grub"
    cp "${BUILD_DIR}/kernel" "${ISO_DIR}/boot/vmlinuz"
    cp "${BUILD_DIR}/initramfs.cpio.gz" "${ISO_DIR}/boot/initrd"
    cp "${SCRIPT_DIR}/iso/boot/grub/grub.cfg" "${ISO_DIR}/boot/grub/"
    
    # Create rootfs squashfs for read-only deployment
    log_info "Creating rootfs squashfs"
    mksquashfs "${SYSROOT}" "${BUILD_DIR}/rootfs.squashfs" -comp xz -Xbcj x86 -q || {
        log_warn "Failed to create squashfs, using cpio instead"
    }
    
    # Also provide cpio as fallback
    cd "${SYSROOT}"
    find . -print0 | cpio -0o -H newc -R 0:0 | gzip > "${BUILD_DIR}/rootfs.cpio.gz"
    cd -
    
    cp "${BUILD_DIR}/rootfs.cpio.gz" "${ISO_DIR}/boot/rootfs.cpio.gz"
    
    # Create ISO with grub-mkrescue
    grub-mkrescue -o "${BUILD_DIR}/fusionos.iso" "${ISO_DIR}" \
        --compress=xz \
        -V "FusionOS" \
        2>/dev/null || {
        log_error "grub-mkrescue failed. Ensuring GRUB tools are installed."
        sudo apt-get update -qq
        sudo apt-get install -y -qq grub-pc xorriso
        grub-mkrescue -o "${BUILD_DIR}/fusionos.iso" "${ISO_DIR}" \
            --compress=xz \
            -V "FusionOS"
    }
    
    log_info "ISO created: ${BUILD_DIR}/fusionos.iso"
    ls -lh "${BUILD_DIR}/fusionos.iso"
}

# Main build flow
main() {
    log_info "Starting FusionOS build"
    log_info "Kernel version: ${KERNEL_VERSION}"
    log_info "BusyBox version: ${BUSYBOX_VERSION}"
    log_info "Build directory: ${BUILD_DIR}"
    
    init_build_dirs
    build_binaries
    setup_fstab
    setup_users
    setup_binfmt
    build_busybox
    build_libs
    build_kernel
    build_initramfs
    build_iso
    
    log_info "Build complete!"
    log_info "Bootable ISO: ${BUILD_DIR}/fusionos.iso"
}

# Parse arguments
if [ $# -eq 0 ]; then
    main
elif [ "$1" = "rootfs" ]; then
    init_build_dirs
    build_binaries
    setup_fstab
    setup_users
    setup_binfmt
    build_busybox
    build_libs
    log_info "Rootfs built in ${SYSROOT}"
elif [ "$1" = "clean" ]; then
    log_info "Cleaning build artifacts"
    rm -rf "${BUILD_DIR}"
else
    log_error "Unknown argument: $1"
    echo "Usage: $0 [rootfs|clean]"
    exit 1
fi
