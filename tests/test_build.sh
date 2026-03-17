#!/bin/bash
# tests/test_build.sh — verify all required source files exist and compile
# correctly after running the build setup steps.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
PASS=0
FAIL=0

# ── Helpers ──────────────────────────────────────────────
check_file() {
    local desc="$1"
    local path="$2"
    if [ -f "${path}" ]; then
        echo "  PASS: ${desc} exists"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc} missing: ${path}"
        FAIL=$((FAIL + 1))
    fi
}

check_executable() {
    local desc="$1"
    local path="$2"
    if [ -x "${path}" ]; then
        echo "  PASS: ${desc} is executable"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc} missing or not executable: ${path}"
        FAIL=$((FAIL + 1))
    fi
}

compile_check() {
    local desc="$1"
    shift
    if gcc "$@" 2>/dev/null; then
        echo "  PASS: ${desc} compiles cleanly"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc} failed to compile"
        gcc "$@" 2>&1 | head -10 || true
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Build / Source File Checks ==="

# ── Source files ─────────────────────────────────────────
check_file "init.c" "${REPO_ROOT}/src/init/init.c"
check_file "initramfs_init.c" "${REPO_ROOT}/src/init/initramfs_init.c"
check_file "shell.c" "${REPO_ROOT}/src/shell/shell.c"
check_file "fusion_run.c" "${REPO_ROOT}/src/compat/fusion_run.c"
check_file "detect.h" "${REPO_ROOT}/src/compat/detect.h"
check_file "detect.c" "${REPO_ROOT}/src/compat/detect.c"
check_file "detect_test.c" "${REPO_ROOT}/src/compat/detect_test.c"
check_file "fusion_pkg.c" "${REPO_ROOT}/src/pkg/fusion_pkg.c"
check_file "monitor.c" "${REPO_ROOT}/src/monitor/monitor.c"

# ── Configuration / script files ─────────────────────────
check_file "kernel/fusionos.config" "${REPO_ROOT}/kernel/fusionos.config"
check_file "iso/boot/grub/grub.cfg" "${REPO_ROOT}/iso/boot/grub/grub.cfg"
check_file "scripts/setup_binfmt.sh" "${REPO_ROOT}/scripts/setup_binfmt.sh"
check_file "run.sh" "${REPO_ROOT}/run.sh"
check_file "build.sh" "${REPO_ROOT}/build.sh"
check_file ".github/workflows/build.yml" "${REPO_ROOT}/.github/workflows/build.yml"

# ── Compilation checks ───────────────────────────────────
mkdir -p "${BUILD_DIR}"

CFLAGS="-O2 -Wall -Wextra -Wpedantic -std=c11"

compile_check "init" ${CFLAGS} -static \
    "${REPO_ROOT}/src/init/init.c" \
    -o "${BUILD_DIR}/init"

compile_check "initramfs_init" ${CFLAGS} -static \
    "${REPO_ROOT}/src/init/initramfs_init.c" \
    -o "${BUILD_DIR}/initramfs_init"

compile_check "fsh" ${CFLAGS} -static \
    "${REPO_ROOT}/src/compat/detect.c" \
    "${REPO_ROOT}/src/shell/shell.c" \
    -o "${BUILD_DIR}/fsh"

compile_check "fusion-run" ${CFLAGS} -static \
    "${REPO_ROOT}/src/compat/detect.c" \
    "${REPO_ROOT}/src/compat/fusion_run.c" \
    -o "${BUILD_DIR}/fusion-run"

compile_check "detect_test" ${CFLAGS} -static \
    "${REPO_ROOT}/src/compat/detect.c" \
    "${REPO_ROOT}/src/compat/detect_test.c" \
    -o "${BUILD_DIR}/detect_test"

compile_check "fusion-pkg" ${CFLAGS} -static \
    "${REPO_ROOT}/src/pkg/fusion_pkg.c" \
    -o "${BUILD_DIR}/fusion-pkg"

compile_check "fusion-monitor" ${CFLAGS} -static \
    "${REPO_ROOT}/src/monitor/monitor.c" \
    -o "${BUILD_DIR}/fusion-monitor"

# ── Built binary checks ──────────────────────────────────
check_executable "init binary" "${BUILD_DIR}/init"
check_executable "fsh binary" "${BUILD_DIR}/fsh"
check_executable "fusion-run binary" "${BUILD_DIR}/fusion-run"
check_executable "fusion-pkg binary" "${BUILD_DIR}/fusion-pkg"
check_executable "fusion-monitor binary" "${BUILD_DIR}/fusion-monitor"

# ── Summary ───────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
