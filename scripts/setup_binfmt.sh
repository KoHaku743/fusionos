#!/bin/sh
# FusionOS binfmt_misc setup
# Registers PE (Windows) and Mach-O (macOS) magic bytes with the kernel's
# binfmt_misc so those binaries are launched transparently without the user
# needing to prefix them with a compatibility-layer command.
#
# Must be run as root after the kernel has mounted binfmt_misc.
# Idempotent: re-running is safe (existing entries are removed first).

set -eu

BINFMT_MISC="/proc/sys/fs/binfmt_misc"

# ---------------------------------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }

# Mount binfmt_misc if it is not already mounted
if [ ! -d "${BINFMT_MISC}" ]; then
    mount -t binfmt_misc none "${BINFMT_MISC}" 2>/dev/null \
        || die "Cannot mount binfmt_misc"
fi

# ---------------------------------------------------------------------------
# Helper: register a binfmt entry.
#   $1 = entry name
#   $2 = registration string (type:name:offset:magic:mask:interp:flags)
register() {
    local name="$1"
    local reg="$2"

    # Remove existing entry with the same name (idempotent)
    if [ -f "${BINFMT_MISC}/${name}" ]; then
        echo -1 > "${BINFMT_MISC}/${name}" 2>/dev/null || true
    fi

    echo "${reg}" > "${BINFMT_MISC}/register" \
        || die "Failed to register binfmt entry: ${name}"

    echo "[binfmt] Registered: ${name}"
}

# ---------------------------------------------------------------------------
# PE32 (Windows 32-bit) — MZ magic + PE signature, routed to wine
# Magic: "MZ" at offset 0
# Mask:  "\xff\xff" — match both bytes exactly
register "fusion-pe32" \
    ":fusion-pe32:M:0:MZ::${WINE32:-/usr/bin/wine}:P"

# PE32+ / PE64 (Windows 64-bit) — same MZ header, routed to wine64
register "fusion-pe64" \
    ":fusion-pe64:M:0:MZ::${WINE64:-/usr/bin/wine64}:P"

# ---------------------------------------------------------------------------
# Mach-O 32-bit big-endian (macOS PPC / legacy Intel)
# Magic: 0xFEEDFACE
register "fusion-macho32be" \
    ":fusion-macho32be:M:0:\xfe\xed\xfa\xce::${DARLING:-/usr/bin/darling}:P"

# Mach-O 32-bit little-endian (macOS Intel 32)
# Magic: 0xCEFAEDFE
register "fusion-macho32le" \
    ":fusion-macho32le:M:0:\xce\xfa\xed\xfe::${DARLING:-/usr/bin/darling}:P"

# Mach-O 64-bit big-endian
# Magic: 0xFEEDFACF
register "fusion-macho64be" \
    ":fusion-macho64be:M:0:\xfe\xed\xfa\xcf::${DARLING:-/usr/bin/darling}:P"

# Mach-O 64-bit little-endian (macOS x86-64 and ARM64)
# Magic: 0xCFFAEDFE
register "fusion-macho64le" \
    ":fusion-macho64le:M:0:\xcf\xfa\xed\xfe::${DARLING:-/usr/bin/darling}:P"

# Mach-O FAT Universal Binary
# Magic: 0xCAFEBABE (big-endian) — note this also matches Java .class files;
# use an 8-byte magic to avoid false positives.
register "fusion-macho-fat" \
    ":fusion-macho-fat:M:0:\xca\xfe\xba\xbe::${DARLING:-/usr/bin/darling}:P"

# ---------------------------------------------------------------------------
echo
echo "FusionOS binfmt_misc setup complete."
echo "PE and Mach-O binaries will now launch transparently."
