#!/bin/bash
# tests/test_detect.sh
# Integration tests for fusion-run binary detection.
# Creates synthetic ELF/PE/Mach-O files with correct magic bytes
# and verifies that detect_test correctly identifies them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DETECT_TEST="${REPO_DIR}/build/detect_test"
TMPDIR_LOCAL="$(mktemp -d)"
PASS=0
FAIL=0

cleanup() { rm -rf "$TMPDIR_LOCAL"; }
trap cleanup EXIT

# Build detect_test if not present
if [ ! -x "$DETECT_TEST" ]; then
    mkdir -p "${REPO_DIR}/build"
    gcc -O2 -static \
        "${REPO_DIR}/src/compat/detect.c" \
        "${REPO_DIR}/src/compat/detect_test.c" \
        -o "$DETECT_TEST" 2>/dev/null || {
        echo "SKIP: could not build detect_test (missing gcc or static libc)" >&2
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

# ---- Create synthetic binaries ----

# ELF x86-64 (e_machine = 0x3e = 62)
create_elf64() {
    local f="$1"
    printf '\x7fELF'      > "$f"   # magic
    printf '\x02'         >> "$f"  # EI_CLASS = ELFCLASS64
    printf '\x01'         >> "$f"  # EI_DATA  = ELFDATA2LSB
    printf '\x01'         >> "$f"  # EI_VERSION
    printf '\x00'         >> "$f"  # EI_OSABI
    printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$f"  # padding
    printf '\x02\x00'     >> "$f"  # e_type = ET_EXEC
    printf '\x3e\x00'     >> "$f"  # e_machine = EM_X86_64 (62)
    # Pad to 64 bytes
    dd if=/dev/zero bs=1 count=44 >> "$f" 2>/dev/null
}

# ELF ARM (e_machine = 0x28 = 40)
create_elf_arm() {
    local f="$1"
    printf '\x7fELF'      > "$f"
    printf '\x01'         >> "$f"  # EI_CLASS = ELFCLASS32
    printf '\x01'         >> "$f"  # EI_DATA  = little-endian
    printf '\x01'         >> "$f"
    printf '\x00'         >> "$f"
    printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$f"
    printf '\x02\x00'     >> "$f"  # e_type
    printf '\x28\x00'     >> "$f"  # e_machine = EM_ARM (40)
    dd if=/dev/zero bs=1 count=44 >> "$f" 2>/dev/null
}

# ELF AArch64 (e_machine = 0xb7 = 183)
create_elf_arm64() {
    local f="$1"
    printf '\x7fELF'      > "$f"
    printf '\x02'         >> "$f"  # ELFCLASS64
    printf '\x01'         >> "$f"  # little-endian
    printf '\x01'         >> "$f"
    printf '\x00'         >> "$f"
    printf '\x00\x00\x00\x00\x00\x00\x00\x00' >> "$f"
    printf '\x02\x00'     >> "$f"
    printf '\xb7\x00'     >> "$f"  # e_machine = EM_AARCH64 (183)
    dd if=/dev/zero bs=1 count=44 >> "$f" 2>/dev/null
}

# PE32+ (64-bit Windows x86-64)
create_pe64() {
    local f="$1"
    # DOS header (64 bytes): MZ signature + pe_offset at 0x3c = 64
    printf 'MZ' > "$f"
    dd if=/dev/zero bs=1 count=58 >> "$f" 2>/dev/null
    printf '\x40\x00\x00\x00' >> "$f"  # pe_offset = 64 at offset 0x3c
    # PE header at offset 64: signature + COFF header + optional header magic
    printf 'PE\x00\x00' >> "$f"   # PE signature
    printf '\x64\x86'   >> "$f"   # Machine = IMAGE_FILE_MACHINE_AMD64 (0x8664)
    dd if=/dev/zero bs=1 count=14 >> "$f" 2>/dev/null
    printf '\x0b\x02'   >> "$f"   # Optional header magic = PE32+ (0x020b)
    dd if=/dev/zero bs=1 count=4 >> "$f" 2>/dev/null
}

# Mach-O 64-bit (little-endian, x86-64)
create_macho64() {
    local f="$1"
    printf '\xcf\xfa\xed\xfe' > "$f"   # MH_MAGIC_64 little-endian
    printf '\x07\x00\x00\x01' >> "$f"  # cputype = CPU_TYPE_X86_64 = 0x01000007
    printf '\x03\x00\x00\x00' >> "$f"  # cpusubtype
    printf '\x02\x00\x00\x00' >> "$f"  # filetype = MH_EXECUTE
    dd if=/dev/zero bs=1 count=16 >> "$f" 2>/dev/null
}

# Mach-O FAT Universal
create_macho_fat() {
    local f="$1"
    printf '\xca\xfe\xba\xbe' > "$f"   # FAT_MAGIC (big-endian)
    printf '\x00\x00\x00\x02' >> "$f"  # nfat_arch = 2
    dd if=/dev/zero bs=1 count=24 >> "$f" 2>/dev/null
}

# ---- Run tests ----

ELF64="${TMPDIR_LOCAL}/test_elf64"
ELFARM="${TMPDIR_LOCAL}/test_elf_arm"
ELFARM64="${TMPDIR_LOCAL}/test_elf_arm64"
PE64="${TMPDIR_LOCAL}/test_pe64"
MACHO64="${TMPDIR_LOCAL}/test_macho64"
MACHOFAT="${TMPDIR_LOCAL}/test_machofat"

create_elf64    "$ELF64"
create_elf_arm  "$ELFARM"
create_elf_arm64 "$ELFARM64"
create_pe64     "$PE64"
create_macho64  "$MACHO64"
create_macho_fat "$MACHOFAT"

# Test ELF x86-64
OUT="$("$DETECT_TEST" "$ELF64" 2>&1)"
assert_contains "ELF x86-64 format"  "$OUT" "ELF"
assert_contains "ELF x86-64 arch"    "$OUT" "x86-64\|x64"

# Test ELF ARM
OUT="$("$DETECT_TEST" "$ELFARM" 2>&1)"
assert_contains "ELF ARM format"     "$OUT" "ELF"
assert_contains "ELF ARM arch"       "$OUT" "ARM"

# Test ELF AArch64
OUT="$("$DETECT_TEST" "$ELFARM64" 2>&1)"
assert_contains "ELF AArch64 format" "$OUT" "ELF"
assert_contains "ELF AArch64 arch"   "$OUT" "ARM64\|AArch64"

# Test PE64
OUT="$("$DETECT_TEST" "$PE64" 2>&1)"
assert_contains "PE64 format"        "$OUT" "PE"
assert_contains "PE64 arch"          "$OUT" "x86-64\|x64"

# Test Mach-O 64
OUT="$("$DETECT_TEST" "$MACHO64" 2>&1)"
assert_contains "Mach-O64 format"    "$OUT" "Mach-O"
assert_contains "Mach-O64 arch"      "$OUT" "x86-64\|x64"

# Test Mach-O FAT
OUT="$("$DETECT_TEST" "$MACHOFAT" 2>&1)"
assert_contains "Mach-O FAT format"  "$OUT" "FAT\|Universal"

# Test unknown format
UNKNOWN="${TMPDIR_LOCAL}/test_unknown"
echo "Hello World" > "$UNKNOWN"
OUT="$("$DETECT_TEST" "$UNKNOWN" 2>&1)" || true
assert_contains "Unknown format detection" "$OUT" "FAILED\|Unknown"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
exit $FAIL
