#!/bin/bash
# FusionOS test: binary format detection
# Creates synthetic ELF/PE/Mach-O files and verifies that fusion-run
# and detect_test report the correct format/architecture.
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
        pass "${label}: found '${expected}'"
    else
        fail "${label}: expected '${expected}' but got: ${actual}"
    fi
}

# ---------------------------------------------------------------------------
# Build detect_test if not already built
if [ ! -x "${BUILD_DIR}/detect_test" ]; then
    echo "Building detect_test..."
    mkdir -p "${BUILD_DIR}"
    gcc -O2 -static \
        -o "${BUILD_DIR}/detect_test" \
        "${SRC_DIR}/compat/detect_test.c" \
        "${SRC_DIR}/compat/detect.c" 2>/dev/null \
        || { echo "ERROR: detect_test build failed"; exit 1; }
fi

DT="${BUILD_DIR}/detect_test"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "=== Binary Detection Tests ==="

# ---------------------------------------------------------------------------
# ELF 32-bit x86 (EM_386 = 3)
{
    # ELF header: magic, EI_CLASS=1 (32-bit), EI_DATA=1 (LE), EI_VERSION=1,
    #             e_type=2 (ET_EXEC), e_machine=3 (EM_386)
    printf '\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${TMP}/elf32_x86"
    printf '\x02\x00\x03\x00' >> "${TMP}/elf32_x86"
    # Pad to 64 bytes so the parser can read e_machine safely
    dd if=/dev/zero bs=1 count=48 >> "${TMP}/elf32_x86" 2>/dev/null
    out=$("${DT}" "${TMP}/elf32_x86" 2>&1 || true)
    assert_contains "ELF32 x86 format"  "ELF"   "${out}"
    assert_contains "ELF32 x86 arch"    "x86"   "${out}"
    assert_contains "ELF32 x86 bits"    "32"    "${out}"
}

# ---------------------------------------------------------------------------
# ELF 64-bit x86-64 (EM_X86_64 = 62)
{
    printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${TMP}/elf64_x64"
    printf '\x02\x00\x3e\x00' >> "${TMP}/elf64_x64"
    dd if=/dev/zero bs=1 count=48 >> "${TMP}/elf64_x64" 2>/dev/null
    out=$("${DT}" "${TMP}/elf64_x64" 2>&1 || true)
    assert_contains "ELF64 x86-64 format"  "ELF"    "${out}"
    assert_contains "ELF64 x86-64 arch"    "x86-64" "${out}"
    assert_contains "ELF64 x86-64 bits"    "64"     "${out}"
}

# ---------------------------------------------------------------------------
# ELF 32-bit ARM (EM_ARM = 40)
{
    printf '\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${TMP}/elf32_arm"
    printf '\x02\x00\x28\x00' >> "${TMP}/elf32_arm"
    dd if=/dev/zero bs=1 count=48 >> "${TMP}/elf32_arm" 2>/dev/null
    out=$("${DT}" "${TMP}/elf32_arm" 2>&1 || true)
    assert_contains "ELF ARM format" "ELF" "${out}"
    assert_contains "ELF ARM arch"   "ARM" "${out}"
}

# ---------------------------------------------------------------------------
# ELF 64-bit AArch64 (EM_AARCH64 = 183 = 0xb7)
{
    printf '\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00' > "${TMP}/elf64_aarch64"
    printf '\x02\x00\xb7\x00' >> "${TMP}/elf64_aarch64"
    dd if=/dev/zero bs=1 count=48 >> "${TMP}/elf64_aarch64" 2>/dev/null
    out=$("${DT}" "${TMP}/elf64_aarch64" 2>&1 || true)
    assert_contains "ELF AArch64 format" "ELF"   "${out}"
    assert_contains "ELF AArch64 arch"   "ARM64" "${out}"
}

# ---------------------------------------------------------------------------
# PE32 (Windows x86) — MZ header
{
    printf 'MZ' > "${TMP}/pe32"
    dd if=/dev/zero bs=1 count=58 >> "${TMP}/pe32" 2>/dev/null
    # e_lfanew at offset 0x3c = 60; set it to 0x40 = 64
    printf '\x40\x00\x00\x00' >> "${TMP}/pe32"
    dd if=/dev/zero bs=1 count=64 >> "${TMP}/pe32" 2>/dev/null
    # PE signature at offset 64: "PE\0\0"
    printf 'PE\x00\x00' >> "${TMP}/pe32"
    # Machine: 0x014c = IMAGE_FILE_MACHINE_I386
    printf '\x4c\x01' >> "${TMP}/pe32"
    dd if=/dev/zero bs=1 count=16 >> "${TMP}/pe32" 2>/dev/null
    out=$("${DT}" "${TMP}/pe32" 2>&1 || true)
    assert_contains "PE32 format" "PE" "${out}"
}

# ---------------------------------------------------------------------------
# PE64 (Windows x86-64) — MZ header
{
    printf 'MZ' > "${TMP}/pe64"
    dd if=/dev/zero bs=1 count=58 >> "${TMP}/pe64" 2>/dev/null
    printf '\x40\x00\x00\x00' >> "${TMP}/pe64"
    dd if=/dev/zero bs=1 count=64 >> "${TMP}/pe64" 2>/dev/null
    printf 'PE\x00\x00' >> "${TMP}/pe64"
    # Machine: 0x8664 = IMAGE_FILE_MACHINE_AMD64 (LE: 0x64, 0x86)
    printf '\x64\x86' >> "${TMP}/pe64"
    dd if=/dev/zero bs=1 count=16 >> "${TMP}/pe64" 2>/dev/null
    out=$("${DT}" "${TMP}/pe64" 2>&1 || true)
    assert_contains "PE64 format" "PE" "${out}"
}

# ---------------------------------------------------------------------------
# Mach-O 64-bit little-endian (macOS x86-64)
# Magic: 0xcffaedfe
{
    printf '\xcf\xfa\xed\xfe' > "${TMP}/macho64le"
    # CPU type: x86-64 = 0x01000007 (LE)
    printf '\x07\x00\x00\x01' >> "${TMP}/macho64le"
    dd if=/dev/zero bs=1 count=24 >> "${TMP}/macho64le" 2>/dev/null
    out=$("${DT}" "${TMP}/macho64le" 2>&1 || true)
    assert_contains "MachO64LE format"   "Mach-O" "${out}"
    assert_contains "MachO64LE 64-bit"   "64"     "${out}"
}

# ---------------------------------------------------------------------------
# Mach-O FAT Universal Binary
# Magic: 0xcafebabe (BE)
{
    printf '\xca\xfe\xba\xbe' > "${TMP}/macho_fat"
    # nfat_arch = 2 (big-endian)
    printf '\x00\x00\x00\x02' >> "${TMP}/macho_fat"
    dd if=/dev/zero bs=1 count=24 >> "${TMP}/macho_fat" 2>/dev/null
    out=$("${DT}" "${TMP}/macho_fat" 2>&1 || true)
    assert_contains "MachO FAT format" "Mach-O\|FAT\|Universal" "${out}"
}

# ---------------------------------------------------------------------------
# Unknown / empty file
{
    printf '\xde\xad\xbe\xef' > "${TMP}/unknown"
    out=$("${DT}" "${TMP}/unknown" 2>&1 || true)
    assert_contains "Unknown format" "unknown\|error\|Unknown" "${out}"
}

# ---------------------------------------------------------------------------
echo
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
[ "${FAIL}" -eq 0 ]
