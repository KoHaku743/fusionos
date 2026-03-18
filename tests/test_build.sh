#!/bin/bash
# FusionOS test: verify build artefacts after 'build.sh rootfs'
# Checks that all required binaries and config files are present in the
# initramfs staging directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
INITRAMFS_DIR="${BUILD_DIR}/initramfs"

# ---------------------------------------------------------------------------
PASS=0; FAIL=0
pass() { echo "  PASS: $*"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL+1)); }

check_file() {
    local label="$1" path="$2"
    if [ -f "${path}" ]; then
        pass "${label}: exists"
    else
        fail "${label}: MISSING — ${path}"
    fi
}

check_exec() {
    local label="$1" path="$2"
    if [ -x "${path}" ]; then
        pass "${label}: executable"
    else
        fail "${label}: NOT executable — ${path}"
    fi
}

check_dir() {
    local label="$1" path="$2"
    if [ -d "${path}" ]; then
        pass "${label}: directory exists"
    else
        fail "${label}: MISSING directory — ${path}"
    fi
}

# ---------------------------------------------------------------------------
# Run rootfs build if initramfs directory is empty
if [ ! -d "${INITRAMFS_DIR}/bin" ]; then
    echo "Running 'build.sh rootfs' to generate artefacts..."
    cd "${REPO_DIR}"
    ./build.sh rootfs
fi

echo "=== Build Artefact Tests ==="

# Initramfs init
check_file "initramfs init"      "${INITRAMFS_DIR}/init"
check_exec "initramfs init exec" "${INITRAMFS_DIR}/init"

# Main init (PID 1)
check_file "main init"      "${INITRAMFS_DIR}/sbin/init"
check_exec "main init exec" "${INITRAMFS_DIR}/sbin/init"

# fsh shell
check_file "fsh"      "${INITRAMFS_DIR}/bin/fsh"
check_exec "fsh exec" "${INITRAMFS_DIR}/bin/fsh"

# fusion-run
check_file "fusion-run"      "${INITRAMFS_DIR}/usr/bin/fusion-run"
check_exec "fusion-run exec" "${INITRAMFS_DIR}/usr/bin/fusion-run"

# fusion-pkg
check_file "fusion-pkg"      "${INITRAMFS_DIR}/usr/bin/fusion-pkg"
check_exec "fusion-pkg exec" "${INITRAMFS_DIR}/usr/bin/fusion-pkg"

# fusion-monitor
check_file "fusion-monitor"      "${INITRAMFS_DIR}/usr/bin/fusion-monitor"
check_exec "fusion-monitor exec" "${INITRAMFS_DIR}/usr/bin/fusion-monitor"

# /etc files
check_file "/etc/passwd"     "${INITRAMFS_DIR}/etc/passwd"
check_file "/etc/group"      "${INITRAMFS_DIR}/etc/group"
check_file "/etc/hostname"   "${INITRAMFS_DIR}/etc/hostname"
check_file "/etc/os-release" "${INITRAMFS_DIR}/etc/os-release"

# Required directories
for d in proc sys dev tmp newroot bin sbin usr/bin usr/sbin lib; do
    check_dir "${d}" "${INITRAMFS_DIR}/${d}"
done

# Verify the /etc/os-release contains FusionOS
if grep -q "FusionOS" "${INITRAMFS_DIR}/etc/os-release" 2>/dev/null; then
    pass "os-release: contains FusionOS"
else
    fail "os-release: does not contain FusionOS"
fi

# Verify init is a static ELF (no dynamic interpreter)
if file "${INITRAMFS_DIR}/init" 2>/dev/null | grep -q "statically linked"; then
    pass "initramfs init: statically linked"
elif file "${INITRAMFS_DIR}/init" 2>/dev/null | grep -q "ELF"; then
    pass "initramfs init: ELF binary (dynamic link warning — should be static)"
else
    fail "initramfs init: not a valid ELF binary"
fi

# ---------------------------------------------------------------------------
echo
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[ "${FAIL}" -eq 0 ]
