#!/bin/sh
# scripts/setup_binfmt.sh
# Register PE (Windows) and Mach-O (macOS) magic bytes with binfmt_misc
# so the kernel can transparently route foreign binaries to fusion-run.
set -e

BINFMT_MISC="/proc/sys/fs/binfmt_misc"
LAUNCHER="/usr/bin/fusion-run"

# Mount binfmt_misc if not already mounted
if [ ! -d "$BINFMT_MISC" ]; then
    mount -t binfmt_misc none "$BINFMT_MISC" || {
        echo "ERROR: Could not mount binfmt_misc" >&2
        exit 1
    }
fi

register() {
    local name="$1"
    local entry="$2"
    local reg_file="$BINFMT_MISC/register"

    # Remove existing entry if present
    if [ -f "$BINFMT_MISC/$name" ]; then
        echo -1 > "$BINFMT_MISC/$name" 2>/dev/null || true
    fi

    echo "$entry" > "$reg_file" && echo "Registered: $name" \
        || echo "Warning: could not register $name"
}

# PE32 / PE64 (Windows): magic bytes "MZ" at offset 0
register "fusion-pe"     ":fusion-pe:M::MZ::${LAUNCHER}:P"

# Mach-O 32-bit little-endian: magic 0xCEFAEDFE
register "fusion-macho32" ":fusion-macho32:M::\xce\xfa\xed\xfe::${LAUNCHER}:P"

# Mach-O 64-bit little-endian: magic 0xCFFAEDFE
register "fusion-macho64" ":fusion-macho64:M::\xcf\xfa\xed\xfe::${LAUNCHER}:P"

# Mach-O FAT Universal: magic 0xCAFEBABE
register "fusion-machofat" ":fusion-machofat:M::\xca\xfe\xba\xbe::${LAUNCHER}:P"

echo "binfmt_misc setup complete."
