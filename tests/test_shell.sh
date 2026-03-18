#!/bin/bash
# FusionOS test: fsh shell built-in commands
# Pipes commands to fsh and checks expected output with grep assertions.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
SRC_DIR="${REPO_DIR}/src"

# ---------------------------------------------------------------------------
PASS=0; FAIL=0
pass() { echo "  PASS: $*"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL+1)); }

assert_contains() {
    local label="$1" expected="$2" actual="$3"
    if echo "${actual}" | grep -qi "${expected}"; then
        pass "${label}"
    else
        fail "${label}: expected '${expected}' in: ${actual}"
    fi
}

assert_not_contains() {
    local label="$1" unexpected="$2" actual="$3"
    if echo "${actual}" | grep -qi "${unexpected}"; then
        fail "${label}: unexpected '${unexpected}' in: ${actual}"
    else
        pass "${label}"
    fi
}

# ---------------------------------------------------------------------------
# Build fsh if not already built
if [ ! -x "${BUILD_DIR}/fsh" ]; then
    echo "Building fsh..."
    mkdir -p "${BUILD_DIR}"
    gcc -O2 \
        -o "${BUILD_DIR}/fsh" \
        "${SRC_DIR}/shell/shell.c" \
        "${SRC_DIR}/compat/detect.c" 2>/dev/null \
        || { echo "ERROR: fsh build failed"; exit 1; }
fi

FSH="${BUILD_DIR}/fsh"

run_fsh() {
    echo "$1" | timeout 5 "${FSH}" 2>&1 || true
}

echo "=== fsh Shell Tests ==="

# ---------------------------------------------------------------------------
# help command
out=$(run_fsh "help")
assert_contains "help: shows FusionOS Shell banner"   "FusionOS Shell"    "${out}"
assert_contains "help: lists exit command"            "exit"              "${out}"
assert_contains "help: lists layers command"          "layers"            "${out}"
assert_contains "help: lists compat command"          "compat"            "${out}"

# ---------------------------------------------------------------------------
# layers command
out=$(run_fsh "layers")
assert_contains "layers: shows Wine"                  "Wine"              "${out}"
assert_contains "layers: shows Darling"               "Darling"           "${out}"
assert_contains "layers: shows Box64"                 "Box64"             "${out}"
assert_contains "layers: shows FEX"                   "FEX\|fex"          "${out}"
assert_contains "layers: shows native ELF"            "native\|ELF"       "${out}"

# ---------------------------------------------------------------------------
# pwd command
out=$(run_fsh "pwd")
assert_contains "pwd: outputs a path starting with /"  "^/"               "${out}"

# ---------------------------------------------------------------------------
# cd and pwd
out=$(run_fsh "$(printf 'cd /tmp\npwd')")
assert_contains "cd+pwd: shows /tmp"                  "/tmp"              "${out}"

# ---------------------------------------------------------------------------
# ls command (should list something or at least not crash)
out=$(run_fsh "ls /")
assert_contains "ls: lists /proc or /tmp or bin"      "proc\|tmp\|bin\|lib" "${out}"

# ---------------------------------------------------------------------------
# history command (add a command first, then check history)
out=$(run_fsh "$(printf 'help\nhistory')")
assert_contains "history: contains prior command"     "help"              "${out}"

# ---------------------------------------------------------------------------
# compat on a missing file — should report error, not crash
out=$(run_fsh "compat /nonexistent_binary_xyz")
assert_contains "compat missing file: reports error"  "error\|not\|Could" "${out}"

# ---------------------------------------------------------------------------
# Unknown command — should not hang
out=$(run_fsh "_no_such_command_xyz_12345")
assert_contains "unknown command: reports not found"  "not found\|No such\|command" "${out}"

# ---------------------------------------------------------------------------
# exit command — fsh should terminate cleanly (exit code 0)
echo "exit" | timeout 5 "${FSH}" > /dev/null 2>&1 && pass "exit: terminates cleanly" \
    || fail "exit: non-zero exit or timeout"

# ---------------------------------------------------------------------------
echo
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[ "${FAIL}" -eq 0 ]
