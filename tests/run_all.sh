#!/bin/bash
# tests/run_all.sh — run all FusionOS integration tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TOTAL_PASS=0
TOTAL_FAIL=0
FAILED_SUITES=()

run_suite() {
    local suite="$1"
    local path="${SCRIPT_DIR}/${suite}"

    if [ ! -f "${path}" ]; then
        echo "WARNING: Test suite not found: ${path}"
        return
    fi

    chmod +x "${path}"

    echo "╔══════════════════════════════════════════════════╗"
    echo "║  Running: ${suite}"
    echo "╚══════════════════════════════════════════════════╝"

    local exit_code=0
    bash "${path}" || exit_code=$?

    if [ "${exit_code}" -eq 0 ]; then
        echo "→ Suite PASSED: ${suite}"
        TOTAL_PASS=$((TOTAL_PASS + 1))
    else
        echo "→ Suite FAILED: ${suite} (exit ${exit_code})"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        FAILED_SUITES+=("${suite}")
    fi
    echo ""
}

echo "╔══════════════════════════════════════════════════╗"
echo "║     FusionOS Integration Test Suite              ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

run_suite "test_build.sh"
run_suite "test_detect.sh"
run_suite "test_shell.sh"

echo "╔══════════════════════════════════════════════════╗"
echo "║  Summary                                         ║"
echo "╠══════════════════════════════════════════════════╣"
printf  "║  Suites passed: %-32d  ║\n" "${TOTAL_PASS}"
printf  "║  Suites failed: %-32d  ║\n" "${TOTAL_FAIL}"
echo "╚══════════════════════════════════════════════════╝"

if [ "${TOTAL_FAIL}" -gt 0 ]; then
    echo ""
    echo "FAILED suites:"
    for s in "${FAILED_SUITES[@]}"; do
        echo "  - ${s}"
    done
    exit 1
fi

echo ""
echo "All test suites passed!"
exit 0
