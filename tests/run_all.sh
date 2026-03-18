#!/bin/bash
# FusionOS test runner
# Runs all test scripts in the tests/ directory and prints a pass/fail summary.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------------------------------------------------------------------------
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SUITES=0
FAILED_SUITES=()

run_suite() {
    local name="$1" script="$2"
    TOTAL_SUITES=$((TOTAL_SUITES+1))

    echo
    echo "══════════════════════════════════════════════════"
    echo "  Running: ${name}"
    echo "══════════════════════════════════════════════════"

    local tmpout
    tmpout="$(mktemp)"
    local rc=0

    bash "${script}" 2>&1 | tee "${tmpout}" || rc=$?

    # Extract pass/fail counts from the summary line
    local p f
    p=$(grep -o '[0-9]* passed' "${tmpout}" | tail -1 | grep -o '[0-9]*' || echo 0)
    f=$(grep -o '[0-9]* failed' "${tmpout}" | tail -1 | grep -o '[0-9]*' || echo 0)
    rm -f "${tmpout}"

    TOTAL_PASS=$((TOTAL_PASS + p))
    TOTAL_FAIL=$((TOTAL_FAIL + f))

    if [ "${rc}" -ne 0 ] || [ "${f}" -gt 0 ]; then
        FAILED_SUITES+=("${name}")
        echo "  → Suite result: FAIL (${p} passed, ${f} failed)"
    else
        echo "  → Suite result: PASS (${p} passed)"
    fi
}

# ---------------------------------------------------------------------------
echo "FusionOS Test Suite"
echo "==================="

run_suite "Binary Detection"  "${SCRIPT_DIR}/test_detect.sh"
run_suite "Shell (fsh)"       "${SCRIPT_DIR}/test_shell.sh"
run_suite "Build Artefacts"   "${SCRIPT_DIR}/test_build.sh"

# ---------------------------------------------------------------------------
echo
echo "══════════════════════════════════════════════════"
echo "  OVERALL RESULTS"
echo "══════════════════════════════════════════════════"
printf "  Total test cases : %d\n" "$((TOTAL_PASS + TOTAL_FAIL))"
printf "  Passed           : %d\n" "${TOTAL_PASS}"
printf "  Failed           : %d\n" "${TOTAL_FAIL}"
printf "  Suites           : %d total" "${TOTAL_SUITES}"

if [ "${#FAILED_SUITES[@]}" -gt 0 ]; then
    echo " (${#FAILED_SUITES[@]} failed)"
    echo
    echo "  Failed suites:"
    for s in "${FAILED_SUITES[@]}"; do
        echo "    - ${s}"
    done
    echo
    exit 1
else
    echo " (all passed)"
    echo
    exit 0
fi
