#!/bin/bash
# tests/test_build.sh
# Verifies that all required files exist after 'build.sh rootfs'.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
SYSROOT="${BUILD_DIR}/sysroot"
PASS=0
FAIL=0

assert_exists() {
    local test_name="$1"
    local path="$2"
    if [ -e "$path" ]; then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name (missing: $path)"
        FAIL=$((FAIL + 1))
    fi
}

assert_executable() {
    local test_name="$1"
    local path="$2"
    if [ -x "$path" ]; then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name (not executable or missing: $path)"
        FAIL=$((FAIL + 1))
    fi
}

assert_size_lt() {
    local test_name="$1"
    local path="$2"
    local max_bytes="$3"
    if [ -f "$path" ]; then
        local size
        size="$(wc -c < "$path")"
        if [ "$size" -lt "$max_bytes" ]; then
            echo "PASS: $test_name (${size} bytes < ${max_bytes})"
            PASS=$((PASS + 1))
        else
            echo "FAIL: $test_name (${size} bytes >= ${max_bytes} limit)"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "FAIL: $test_name (file missing: $path)"
        FAIL=$((FAIL + 1))
    fi
}

# If no rootfs build found, run it first
if [ ! -d "$SYSROOT" ]; then
    echo "[*] Sysroot not found, running build.sh rootfs..."
    cd "$REPO_DIR"
    bash build.sh rootfs
fi

echo "[*] Checking build output in: $SYSROOT"
echo ""

# Core init binaries
assert_executable "init (PID 1)"            "${SYSROOT}/sbin/init"
assert_executable "fsh shell"               "${SYSROOT}/bin/fsh"
assert_executable "fusion-run launcher"     "${SYSROOT}/usr/bin/fusion-run"
assert_executable "fusion-pkg manager"      "${SYSROOT}/usr/bin/fusion-pkg"
assert_executable "fusion-monitor TUI"      "${SYSROOT}/usr/bin/fusion-monitor"

# initramfs_init (in build dir, not sysroot)
assert_executable "initramfs_init binary"   "${BUILD_DIR}/initramfs_init"

# detect_test utility
assert_executable "detect_test utility"     "${BUILD_DIR}/detect_test"

# System configuration files
assert_exists "fstab"                       "${SYSROOT}/etc/fstab"
assert_exists "passwd"                      "${SYSROOT}/etc/passwd"
assert_exists "group"                       "${SYSROOT}/etc/group"
assert_exists "binfmt init script"          "${SYSROOT}/etc/init.d/binfmt"

# Required directories
assert_exists "sysroot /proc"               "${SYSROOT}/proc"
assert_exists "sysroot /sys"                "${SYSROOT}/sys"
assert_exists "sysroot /dev"                "${SYSROOT}/dev"
assert_exists "sysroot /tmp"                "${SYSROOT}/tmp"
assert_exists "sysroot /var/lib/fusion-pkg" "${SYSROOT}/var/lib/fusion-pkg"

# initramfs_init must be under 100KB (as per spec)
assert_size_lt "initramfs_init under 100KB" "${BUILD_DIR}/initramfs_init" 102400

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
exit $FAIL
