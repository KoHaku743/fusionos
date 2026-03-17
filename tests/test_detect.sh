#!/bin/bash
# tests/test_detect.sh — integration tests for binary format detection
# Creates synthetic binary files with correct magic bytes and verifies
# that detect_test correctly identifies them.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
DETECT_TEST="${BUILD_DIR}/detect_test"
TMPDIR_TEST="${TMPDIR:-/tmp}/fusion_detect_test_$$"
PASS=0
FAIL=0

cleanup() { rm -rf "${TMPDIR_TEST}"; }
trap cleanup EXIT

mkdir -p "${TMPDIR_TEST}"

# ── Helpers ──────────────────────────────────────────────
assert_contains() {
    local desc="$1"
    local file="$2"
    local expected="$3"
    local output
    output=$("${DETECT_TEST}" "${file}" 2>&1) || true
    if echo "${output}" | grep -q "${expected}"; then
        echo "  PASS: ${desc}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}"
        echo "        Expected output to contain: ${expected}"
        echo "        Actual output: ${output}"
        FAIL=$((FAIL + 1))
    fi
}

# ── Build detect_test if needed ───────────────────────────
if [ ! -x "${DETECT_TEST}" ]; then
    echo "Building detect_test..."
    mkdir -p "${BUILD_DIR}"
    gcc -O2 -Wall -Wextra -std=c11 -static \
        "${REPO_ROOT}/src/compat/detect.c" \
        "${REPO_ROOT}/src/compat/detect_test.c" \
        -o "${DETECT_TEST}"
fi

echo "=== Binary Detection Tests ==="

# ── ELF x86 (32-bit) ─────────────────────────────────────
ELF32="${TMPDIR_TEST}/elf32.bin"
# Magic: 7f 45 4c 46, EI_CLASS=1 (32-bit), EI_DATA=1 (LE), e_machine=3 (i386)
printf '\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${ELF32}"
printf '\x02\x00\x03\x00\x01\x00\x00\x00\x00\x00\x00\x00' >> "${ELF32}"
# Pad to 64 bytes
dd if=/dev/zero bs=1 count=36 >> "${ELF32}" 2>/dev/null
assert_contains "ELF 32-bit x86 detected" "${ELF32}" "x86"

# ── ELF x86-64 (64-bit) ──────────────────────────────────
ELF64="${TMPDIR_TEST}/elf64.bin"
# Magic: 7f 45 4c 46, EI_CLASS=2 (64-bit), EI_DATA=1 (LE), e_machine=62 (x86_64)
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${ELF64}"
printf '\x02\x00\x3e\x00\x01\x00\x00\x00\x00\x00\x00\x00' >> "${ELF64}"
dd if=/dev/zero bs=1 count=36 >> "${ELF64}" 2>/dev/null
assert_contains "ELF 64-bit x86-64 detected" "${ELF64}" "x86"

# ── ELF ARM (32-bit) ─────────────────────────────────────
ELFARM="${TMPDIR_TEST}/elfarm.bin"
# e_machine = 40 (0x28) = EM_ARM
printf '\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${ELFARM}"
printf '\x02\x00\x28\x00\x01\x00\x00\x00\x00\x00\x00\x00' >> "${ELFARM}"
dd if=/dev/zero bs=1 count=36 >> "${ELFARM}" 2>/dev/null
assert_contains "ELF 32-bit ARM detected" "${ELFARM}" "ARM"

# ── ELF AArch64 (64-bit) ─────────────────────────────────
ELFARM64="${TMPDIR_TEST}/elfarm64.bin"
# e_machine = 183 (0xb7) = EM_AARCH64
printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${ELFARM64}"
printf '\x02\x00\xb7\x00\x01\x00\x00\x00\x00\x00\x00\x00' >> "${ELFARM64}"
dd if=/dev/zero bs=1 count=36 >> "${ELFARM64}" 2>/dev/null
assert_contains "ELF 64-bit AArch64 detected" "${ELFARM64}" "ARM"

# ── PE32 (Windows 32-bit) ────────────────────────────────
PE32="${TMPDIR_TEST}/pe32.exe"
{
    # DOS header: MZ magic + 58 padding bytes + PE offset at 0x3c = 64 = 0x40
    printf 'MZ'
    dd if=/dev/zero bs=1 count=58 2>/dev/null
    printf '\x40\x00\x00\x00'     # e_lfanew = 0x40 (64)
    # PE signature at offset 64
    printf 'PE\x00\x00'           # PE signature
    printf '\x4c\x01'             # Machine = 0x014c (i386)
    dd if=/dev/zero bs=1 count=2 2>/dev/null
    printf '\x00\x00\x00\x00'     # TimeDateStamp
    printf '\x00\x00\x00\x00'     # PointerToSymbolTable
    printf '\x00\x00\x00\x00'     # NumberOfSymbols
    printf '\x00\x00'             # SizeOfOptionalHeader = 0 (we'll use 0xe0)
    printf '\x00\x00'             # Characteristics
    # Optional header magic: 0x010b = PE32
    printf '\x0b\x01'
    dd if=/dev/zero bs=1 count=16 2>/dev/null
} > "${PE32}"
assert_contains "PE 32-bit Windows detected" "${PE32}" "Windows"

# ── PE32+ (Windows 64-bit) ───────────────────────────────
PE64="${TMPDIR_TEST}/pe64.exe"
{
    printf 'MZ'
    dd if=/dev/zero bs=1 count=58 2>/dev/null
    printf '\x40\x00\x00\x00'
    printf 'PE\x00\x00'
    printf '\x64\x86'             # Machine = 0x8664 (x86-64)
    dd if=/dev/zero bs=1 count=2 2>/dev/null
    printf '\x00\x00\x00\x00'
    printf '\x00\x00\x00\x00'
    printf '\x00\x00\x00\x00'
    printf '\x00\x00'
    printf '\x00\x00'
    printf '\x0b\x02'             # Optional header magic = PE32+
    dd if=/dev/zero bs=1 count=16 2>/dev/null
} > "${PE64}"
assert_contains "PE 64-bit Windows detected" "${PE64}" "Windows"

# ── Mach-O 32-bit ────────────────────────────────────────
MACHO32="${TMPDIR_TEST}/macho32.bin"
# feedface (little-endian), cpu_type=7 (i386)
printf '\xce\xfa\xed\xfe\x07\x00\x00\x00' > "${MACHO32}"
dd if=/dev/zero bs=1 count=24 >> "${MACHO32}" 2>/dev/null
assert_contains "Mach-O 32-bit detected" "${MACHO32}" "Mach-O"

# ── Mach-O 64-bit ────────────────────────────────────────
MACHO64="${TMPDIR_TEST}/macho64.bin"
# feedfacf (little-endian), cpu_type=0x01000007 (x86_64)
printf '\xcf\xfa\xed\xfe\x07\x00\x00\x01' > "${MACHO64}"
dd if=/dev/zero bs=1 count=24 >> "${MACHO64}" 2>/dev/null
assert_contains "Mach-O 64-bit detected" "${MACHO64}" "Mach-O"

# ── Mach-O FAT Universal ─────────────────────────────────
MACHOFAT="${TMPDIR_TEST}/machofat.bin"
# cafebabe (big-endian fat magic), nfat_arch=2
printf '\xca\xfe\xba\xbe\x00\x00\x00\x02' > "${MACHOFAT}"
dd if=/dev/zero bs=1 count=24 >> "${MACHOFAT}" 2>/dev/null
assert_contains "Mach-O FAT Universal detected" "${MACHOFAT}" "FAT"

# ── Unknown format ───────────────────────────────────────
UNKNOWN="${TMPDIR_TEST}/unknown.bin"
printf '\xde\xad\xbe\xef' > "${UNKNOWN}"
output=$("${DETECT_TEST}" "${UNKNOWN}" 2>&1) || true
if echo "${output}" | grep -qi "unknown\|failed"; then
    echo "  PASS: Unknown format handled gracefully"
    PASS=$((PASS + 1))
else
    echo "  FAIL: Expected graceful failure for unknown format"
    echo "        Got: ${output}"
    FAIL=$((FAIL + 1))
fi

# ── Summary ───────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
