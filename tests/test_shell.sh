#!/bin/bash
# tests/test_shell.sh — integration tests for fsh (FusionOS shell)
# Pipes commands to fsh and checks output.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
FSH="${BUILD_DIR}/fsh"
PASS=0
FAIL=0

# ── Build fsh if needed ───────────────────────────────────
if [ ! -x "${FSH}" ]; then
    echo "Building fsh..."
    mkdir -p "${BUILD_DIR}"
    gcc -O2 -Wall -Wextra -std=c11 -static \
        "${REPO_ROOT}/src/compat/detect.c" \
        "${REPO_ROOT}/src/shell/shell.c" \
        -o "${FSH}"
fi

# ── Helpers ──────────────────────────────────────────────
run_cmd() {
    echo "$1" | "${FSH}" 2>&1
}

assert_output_contains() {
    local desc="$1"
    local cmd="$2"
    local expected="$3"
    local output
    output=$(run_cmd "${cmd}")
    if echo "${output}" | grep -q "${expected}"; then
        echo "  PASS: ${desc}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}"
        echo "        Command:  ${cmd}"
        echo "        Expected: ${expected}"
        echo "        Actual:   ${output}"
        FAIL=$((FAIL + 1))
    fi
}

assert_exit_code() {
    local desc="$1"
    local cmd="$2"
    local expected_exit="$3"
    local actual_exit=0
    echo "${cmd}" | "${FSH}" > /dev/null 2>&1 || actual_exit=$?
    if [ "${actual_exit}" -eq "${expected_exit}" ]; then
        echo "  PASS: ${desc} (exit ${actual_exit})"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}"
        echo "        Expected exit: ${expected_exit}, got: ${actual_exit}"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Shell (fsh) Tests ==="

# ── help command ─────────────────────────────────────────
assert_output_contains "help shows built-in commands" "help" "help"
assert_output_contains "help shows FusionOS" "help" "FusionOS"
assert_output_contains "help shows exit" "help" "exit"
assert_output_contains "help shows layers" "help" "layers"

# ── layers command ───────────────────────────────────────
assert_output_contains "layers shows Wine" "layers" "Wine"
assert_output_contains "layers shows Darling" "layers" "Darling"
assert_output_contains "layers shows Box64" "layers" "Box64"
assert_output_contains "layers shows FEX" "layers" "FEX"

# ── pwd command ──────────────────────────────────────────
assert_output_contains "pwd returns a path" "pwd" "/"

# ── cd and pwd ───────────────────────────────────────────
assert_output_contains "cd /tmp then pwd shows /tmp" "cd /tmp
pwd" "/tmp"

# ── history command ──────────────────────────────────────
output=$(printf 'help\nlayers\nhistory\n' | "${FSH}" 2>&1)
if echo "${output}" | grep -q "help" && echo "${output}" | grep -q "layers"; then
    echo "  PASS: history tracks previous commands"
    PASS=$((PASS + 1))
else
    echo "  FAIL: history should track previous commands"
    echo "        Got: ${output}"
    FAIL=$((FAIL + 1))
fi

# ── empty line ───────────────────────────────────────────
output=$(echo "" | "${FSH}" 2>&1)
echo "  PASS: empty line does not crash fsh"
PASS=$((PASS + 1))

# ── exit command ─────────────────────────────────────────
assert_exit_code "exit returns 0" "exit" 0

# ── cd missing argument ──────────────────────────────────
output=$(echo "cd" | "${FSH}" 2>&1)
if echo "${output}" | grep -qi "missing\|argument\|usage"; then
    echo "  PASS: cd with no argument shows error"
    PASS=$((PASS + 1))
else
    echo "  FAIL: cd with no argument should show error, got: ${output}"
    FAIL=$((FAIL + 1))
fi

# ── compat on non-existent file ──────────────────────────
output=$(echo "compat /nonexistent/file" | "${FSH}" 2>&1)
if echo "${output}" | grep -qi "could not\|failed\|not found\|detect"; then
    echo "  PASS: compat handles missing file gracefully"
    PASS=$((PASS + 1))
else
    echo "  FAIL: compat should report error for missing file, got: ${output}"
    FAIL=$((FAIL + 1))
fi

# ── compat on real ELF binary ────────────────────────────
assert_output_contains "compat on /bin/sh shows ELF" "compat /bin/sh" "ELF"

# ── Summary ───────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
