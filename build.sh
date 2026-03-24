#!/bin/sh
# FusionOS userspace build script
# Compiles all C components into a staging directory (out/)

set -e

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE}"
STAGING="out"

die() { echo "ERROR: $*" >&2; exit 1; }

mkdir -p "${STAGING}/bin"

echo "=== Building FusionOS userspace components ==="

# ── compat / binary-format detection library ──────────────────────────
echo "[1/5] Compiling compat/detect.c..."
"${CC}" ${CFLAGS} -c src/compat/detect.c -o "${STAGING}/detect.o"

# ── detect_test utility ───────────────────────────────────────────────
echo "[2/5] Linking detect-test..."
"${CC}" ${CFLAGS} src/compat/detect_test.c "${STAGING}/detect.o" \
    -o "${STAGING}/bin/detect-test"

# ── fusion-run universal launcher ────────────────────────────────────
echo "[3/5] Linking fusion-run..."
"${CC}" ${CFLAGS} src/compat/fusion_run.c "${STAGING}/detect.o" \
    -o "${STAGING}/bin/fusion-run"

# ── fsh (FusionOS shell) ──────────────────────────────────────────────
echo "[4/5] Linking fsh..."
"${CC}" ${CFLAGS} src/shell/shell.c "${STAGING}/detect.o" \
    -o "${STAGING}/bin/fsh"

# ── fusion-pkg (package manager) ─────────────────────────────────────
echo "[5/5] Linking fusion-pkg..."
"${CC}" ${CFLAGS} src/pkg/fusion_pkg.c \
    -o "${STAGING}/bin/fusion-pkg"

# ── system monitor ────────────────────────────────────────────────────
echo "[+] Linking fusion-monitor..."
"${CC}" ${CFLAGS} src/monitor/monitor.c \
    -o "${STAGING}/bin/fusion-monitor"

# ── init (PID 1) ──────────────────────────────────────────────────────
echo "[+] Linking init..."
"${CC}" ${CFLAGS} src/init/init.c \
    -o "${STAGING}/bin/init"

# ── initramfs init ────────────────────────────────────────────────────
echo "[+] Linking initramfs-init..."
"${CC}" ${CFLAGS} src/init/initramfs_init.c \
    -o "${STAGING}/bin/initramfs-init"

echo ""
echo "=== Build complete. Binaries in ${STAGING}/bin/ ==="
ls -lh "${STAGING}/bin/"
