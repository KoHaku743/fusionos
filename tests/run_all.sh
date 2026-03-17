#!/bin/bash
# tests/run_all.sh
# Run all FusionOS integration tests and print a pass/fail summary.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TESTS=(
    "${SCRIPT_DIR}/test_detect.sh"
    "${SCRIPT_DIR}/test_shell.sh"
    "${SCRIPT_DIR}/test_build.sh"
)

TOTAL_PASS=0
TOTAL_FAIL=0
SUITE_PASS=0
SUITE_FAIL=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=============================="
echo " FusionOS Integration Tests"
echo "=============================="
echo ""

for test_script in "${TESTS[@]}"; do
    test_name="$(basename "$test_script")"
    echo "--- Running: $test_name ---"

    if [ ! -f "$test_script" ]; then
        echo -e "${YELLOW}SKIP${NC}: $test_name (not found)"
        continue
    fi

    chmod +x "$test_script"

    # Run the test, capture output and exit code
    set +e
    output="$(bash "$test_script" 2>&1)"
    exit_code=$?
    set -e

    echo "$output"

    # Parse pass/fail counts from output line "Results: N passed, N failed"
    pass_count="$(echo "$output" | grep -oP '\d+ passed' | grep -oP '\d+' || echo 0)"
    fail_count="$(echo "$output" | grep -oP '\d+ failed' | grep -oP '\d+' || echo 0)"
    TOTAL_PASS=$((TOTAL_PASS + pass_count))
    TOTAL_FAIL=$((TOTAL_FAIL + fail_count))

    if [ "$exit_code" -eq 0 ]; then
        echo -e "${GREEN}SUITE PASS${NC}: $test_name"
        SUITE_PASS=$((SUITE_PASS + 1))
    else
        echo -e "${RED}SUITE FAIL${NC}: $test_name (exit code: $exit_code)"
        SUITE_FAIL=$((SUITE_FAIL + 1))
    fi
    echo ""
done

echo "=============================="
echo " Summary"
echo "=============================="
echo "  Test suites : ${SUITE_PASS} passed, ${SUITE_FAIL} failed"
echo "  Assertions  : ${TOTAL_PASS} passed, ${TOTAL_FAIL} failed"

if [ "$SUITE_FAIL" -eq 0 ] && [ "$TOTAL_FAIL" -eq 0 ]; then
    echo -e "${GREEN}ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}SOME TESTS FAILED${NC}"
    exit 1
fi
