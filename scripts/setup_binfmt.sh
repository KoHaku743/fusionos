#!/bin/sh
# setup_binfmt.sh — register PE (Windows) and Mach-O (macOS) binary formats
# with the Linux kernel's binfmt_misc subsystem so they are transparently
# routed to fusion-run on execution.

set -e

BINFMT_MISC=/proc/sys/fs/binfmt_misc
LAUNCHER=/usr/bin/fusion-run

# Mount binfmt_misc if not already mounted
if [ ! -d "${BINFMT_MISC}" ]; then
    mount -t binfmt_misc none /proc/sys/fs/binfmt_misc
fi

if [ ! -f "${BINFMT_MISC}/register" ]; then
    echo "ERROR: binfmt_misc not available" >&2
    exit 1
fi

if [ ! -x "${LAUNCHER}" ]; then
    echo "ERROR: ${LAUNCHER} not found or not executable" >&2
    exit 1
fi

register() {
    local name="$1"
    local entry="$2"
    # Remove stale entry if it exists
    if [ -f "${BINFMT_MISC}/${name}" ]; then
        echo -1 > "${BINFMT_MISC}/${name}"
    fi
    echo "${entry}" > "${BINFMT_MISC}/register"
    echo "Registered binfmt: ${name}"
}

# PE32 / PE64 (Windows): magic bytes 'MZ' at offset 0
register wine       ":wine:M::MZ::${LAUNCHER}:OC"

# Mach-O 32-bit little-endian  (feedface)
register macho32    ":macho32:M::\xce\xfa\xed\xfe::${LAUNCHER}:OC"

# Mach-O 64-bit little-endian  (feedfacf)
register macho64    ":macho64:M::\xcf\xfa\xed\xfe::${LAUNCHER}:OC"

# Mach-O FAT universal         (cafebabe)
register machofat   ":machofat:M::\xca\xfe\xba\xbe::${LAUNCHER}:OC"

echo "binfmt_misc registration complete."
