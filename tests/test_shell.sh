#!/bin/bash
# tests/test_shell.sh
# Integration tests for fsh (FusionOS shell).
# Pipes commands to fsh and checks output with grep assertions.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FSH="${REPO_DIR}/build/fsh"
PASS=0
FAIL=0

# Build fsh if not present
if [ ! -x "$FSH" ]; then
    mkdir -p "${REPO_DIR}/build"
    gcc -O2 -static \
        "${REPO_DIR}/src/compat/detect.c" \
        "${REPO_DIR}/src/shell/shell.c" \
        -o "$FSH" 2>/dev/null || {
        echo "SKIP: could not build fsh (missing gcc or static libc)" >&2
        exit 0
    }
fi

assert_contains() {
    local test_name="$1"
    local output="$2"
    local expected="$3"
    if echo "$output" | grep -q "$expected"; then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name"
        echo "  Expected output to contain: $expected"
        echo "  Got: $output"
        FAIL=$((FAIL + 1))
    fi
}

assert_not_contains() {
    local test_name="$1"
    local output="$2"
    local unexpected="$3"
    if ! echo "$output" | grep -q "$unexpected"; then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name"
        echo "  Expected output NOT to contain: $unexpected"
        echo "  Got: $output"
        FAIL=$((FAIL + 1))
    fi
}

run_fsh() {
    echo "$1" | timeout 5 "$FSH" 2>&1 || true
}

# Test: help command shows expected sections
OUT="$(run_fsh 'help')"
assert_contains "help shows shell name"    "$OUT" "FusionOS Shell\|fsh"
assert_contains "help shows built-ins"     "$OUT" "Built-in\|help"
assert_contains "help shows exit"          "$OUT" "exit"
assert_contains "help shows cd"            "$OUT" "cd"
assert_contains "help shows history"       "$OUT" "history"
assert_contains "help shows layers"        "$OUT" "layers"

# Test: layers command
OUT="$(run_fsh 'layers')"
assert_contains "layers shows Wine"        "$OUT" "[Ww]ine"
assert_contains "layers shows Darling"     "$OUT" "[Dd]arling"
assert_contains "layers shows Box64"       "$OUT" "[Bb]ox64"
assert_contains "layers shows FEX"         "$OUT" "FEX\|[Ff]ex"

# Test: pwd outputs a path
OUT="$(run_fsh 'pwd')"
assert_contains "pwd outputs path"         "$OUT" "/"

# Test: cd and pwd
OUT="$(run_fsh "$(printf 'cd /tmp\npwd')")"
assert_contains "cd changes directory"     "$OUT" "/tmp"

# Test: history records commands
OUT="$(run_fsh "$(printf 'help\npwd\nhistory')")"
assert_contains "history records help"     "$OUT" "help"
assert_contains "history records pwd"      "$OUT" "pwd"

# Test: unknown command gives error
OUT="$(run_fsh 'this_cmd_does_not_exist_fusionos_test')"
assert_contains "unknown command error"    "$OUT" "not found\|command\|error"

# Test: exit command exits
OUT="$(run_fsh 'exit')"
# fsh should exit cleanly (no crash output expected)
assert_not_contains "exit no crash"        "$OUT" "Segfault\|segfault\|core dumped"

# Test: compat command on a real binary
OUT="$(run_fsh "compat /bin/sh")"
assert_contains "compat detects ELF"       "$OUT" "ELF\|elf"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
exit $FAIL
