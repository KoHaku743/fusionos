#include "detect.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ── ELF constants ─────────────────────────────────────────────────── */
#define ELF_CLASS_32      1
#define ELF_CLASS_64      2
#define ELF_DATA_LSB      1   /* little-endian */
#define ELF_DATA_MSB      2   /* big-endian    */

#define EM_386        3
#define EM_PPC        20
#define EM_ARM        40
#define EM_X86_64     62
#define EM_AARCH64    183
#define EM_RISCV      243

/* ── Mach-O constants ──────────────────────────────────────────────── */
#define MACHO_MAGIC_32    0xFEEDFACEU   /* 32-bit native          */
#define MACHO_MAGIC_64    0xFEEDFACFU   /* 64-bit native          */
#define MACHO_CIGAM_32    0xCEFAEDFEU   /* 32-bit byte-swapped    */
#define MACHO_CIGAM_64    0xCFFAEDFEU   /* 64-bit byte-swapped    */
#define MACHO_FAT_MAGIC   0xCAFEBABEU   /* FAT universal binary   */
#define MACHO_FAT_CIGAM   0xBEBAFECAU   /* FAT byte-swapped       */

/* Mach-O cputype field values */
#define CPU_TYPE_X86      7
#define CPU_TYPE_X86_64   0x01000007u
#define CPU_TYPE_ARM      12
#define CPU_TYPE_ARM64    0x0100000Cu
#define CPU_TYPE_POWERPC  18

/* ── PE constants ──────────────────────────────────────────────────── */
#define IMAGE_FILE_MACHINE_I386   0x014c
#define IMAGE_FILE_MACHINE_AMD64  0x8664
#define IMAGE_FILE_MACHINE_ARM    0x01c0
#define IMAGE_FILE_MACHINE_ARM64  0xaa64

/* ── Internal helpers ──────────────────────────────────────────────── */

/**
 * Read up to `size` bytes from `fd` starting at `offset`.
 * Returns bytes read, or -1 on error.
 */
static ssize_t pread_exact(int fd, void *buf, size_t size, off_t offset) {
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;

    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, (char *)buf + total, size - total);
        if (n == 0) break;
        if (n < 0) return -1;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* ── ELF detection ─────────────────────────────────────────────────── */

int detect_elf(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* Read ELF ident (16 bytes) + e_type (2) + e_machine (2) */
    uint8_t hdr[20];
    if (pread_exact(fd, hdr, sizeof(hdr), 0) < (ssize_t)sizeof(hdr)) {
        close(fd);
        return -1;
    }
    close(fd);

    /* Verify ELF magic */
    if (hdr[0] != 0x7f || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
        return -1;
    }

    out->fmt = BIN_ELF;

    uint8_t ei_class = hdr[4];
    out->bits = (ei_class == ELF_CLASS_64) ? 64 : 32;

    uint8_t ei_data = hdr[5];
    out->is_little_endian = (ei_data == ELF_DATA_LSB) ? 1 : 0;

    /* e_machine is at offset 18 (after 16-byte ident + 2-byte e_type) */
    uint16_t e_machine;
    if (out->is_little_endian) {
        e_machine = read_le16(&hdr[18]);
    } else {
        e_machine = (uint16_t)((hdr[18] << 8) | hdr[19]);
    }

    switch (e_machine) {
        case EM_386:
            out->arch = ARCH_X86;
            snprintf(out->description, sizeof(out->description),
                     "ELF 32-bit x86 Linux executable");
            out->launcher = NULL;
            out->launcher_args[0] = NULL;
            break;

        case EM_X86_64:
            out->arch = ARCH_X86_64;
            snprintf(out->description, sizeof(out->description),
                     "ELF 64-bit x86-64 Linux executable");
            out->launcher = NULL;
            out->launcher_args[0] = NULL;
            break;

        case EM_ARM:
            out->arch = ARCH_ARM;
            snprintf(out->description, sizeof(out->description),
                     "ELF 32-bit ARM Linux executable");
            out->launcher         = "/usr/bin/box86";
            out->launcher_args[0] = "/usr/bin/box86";
            out->launcher_args[1] = NULL;
            break;

        case EM_AARCH64:
            out->arch = ARCH_ARM64;
            snprintf(out->description, sizeof(out->description),
                     "ELF 64-bit AArch64 Linux executable");
            out->launcher         = "/usr/bin/fex-emu";
            out->launcher_args[0] = "/usr/bin/fex-emu";
            out->launcher_args[1] = NULL;
            break;

        case EM_RISCV:
            out->arch = (out->bits == 64) ? ARCH_RISCV64 : ARCH_RISCV;
            snprintf(out->description, sizeof(out->description),
                     "ELF %d-bit RISC-V Linux executable", out->bits);
            out->launcher = NULL;
            out->launcher_args[0] = NULL;
            break;

        default:
            out->arch = ARCH_UNKNOWN;
            snprintf(out->description, sizeof(out->description),
                     "ELF executable (machine=0x%04x)", (unsigned)e_machine);
            out->launcher = NULL;
            out->launcher_args[0] = NULL;
            break;
    }

    return 0;
}

/* ── PE detection ──────────────────────────────────────────────────── */

int detect_pe(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    /* Read DOS header (64 bytes).  Offset 0x3C holds the PE header offset. */
    uint8_t dos_hdr[64];
    if (pread_exact(fd, dos_hdr, sizeof(dos_hdr), 0) < (ssize_t)sizeof(dos_hdr)) {
        close(fd);
        return -1;
    }

    /* Check "MZ" magic */
    if (dos_hdr[0] != 'M' || dos_hdr[1] != 'Z') {
        close(fd);
        return -1;
    }

    uint32_t pe_offset = read_le32(&dos_hdr[0x3C]);
    if (pe_offset < 64 || pe_offset > 0x100000) {
        close(fd);
        return -1;
    }

    /* Read PE signature (4 bytes) + COFF Machine field (2 bytes) */
    uint8_t pe_hdr[6];
    if (pread_exact(fd, pe_hdr, sizeof(pe_hdr), (off_t)pe_offset) < (ssize_t)sizeof(pe_hdr)) {
        close(fd);
        return -1;
    }
    close(fd);

    /* Verify "PE\0\0" signature */
    if (pe_hdr[0] != 'P' || pe_hdr[1] != 'E' || pe_hdr[2] != 0 || pe_hdr[3] != 0) {
        return -1;
    }

    uint16_t machine = read_le16(&pe_hdr[4]);

    out->fmt              = BIN_PE;
    out->is_little_endian = 1;

    switch (machine) {
        case IMAGE_FILE_MACHINE_I386:
            out->arch = ARCH_X86;
            out->bits = 32;
            snprintf(out->description, sizeof(out->description),
                     "PE32 Windows executable (x86)");
            out->launcher         = "/usr/bin/wine";
            out->launcher_args[0] = "/usr/bin/wine";
            out->launcher_args[1] = NULL;
            break;

        case IMAGE_FILE_MACHINE_AMD64:
            out->arch = ARCH_X86_64;
            out->bits = 64;
            snprintf(out->description, sizeof(out->description),
                     "PE32+ Windows executable (x86-64)");
            out->launcher         = "/usr/bin/wine64";
            out->launcher_args[0] = "/usr/bin/wine64";
            out->launcher_args[1] = NULL;
            break;

        case IMAGE_FILE_MACHINE_ARM:
            out->arch = ARCH_ARM;
            out->bits = 32;
            snprintf(out->description, sizeof(out->description),
                     "PE32 Windows executable (ARM)");
            out->launcher         = "/usr/bin/wine";
            out->launcher_args[0] = "/usr/bin/wine";
            out->launcher_args[1] = NULL;
            break;

        case IMAGE_FILE_MACHINE_ARM64:
            out->arch = ARCH_ARM64;
            out->bits = 64;
            snprintf(out->description, sizeof(out->description),
                     "PE32+ Windows executable (ARM64)");
            out->launcher         = "/usr/bin/wine64";
            out->launcher_args[0] = "/usr/bin/wine64";
            out->launcher_args[1] = NULL;
            break;

        default:
            out->arch = ARCH_UNKNOWN;
            out->bits = 0;
            snprintf(out->description, sizeof(out->description),
                     "PE Windows executable (machine=0x%04x)", (unsigned)machine);
            out->launcher         = "/usr/bin/wine";
            out->launcher_args[0] = "/usr/bin/wine";
            out->launcher_args[1] = NULL;
            break;
    }

    return 0;
}

/* ── Mach-O detection ──────────────────────────────────────────────── */

int detect_macho(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    uint8_t buf[8];
    if (pread_exact(fd, buf, sizeof(buf), 0) < (ssize_t)sizeof(buf)) {
        close(fd);
        return -1;
    }
    close(fd);

    uint32_t magic = read_le32(buf);

    /* FAT universal binary */
    if (magic == MACHO_FAT_MAGIC || magic == MACHO_FAT_CIGAM) {
        out->fmt              = BIN_MACHO_FAT;
        out->arch             = ARCH_UNKNOWN;
        out->bits             = 0;
        out->is_little_endian = 0;
        snprintf(out->description, sizeof(out->description),
                 "Mach-O universal (FAT) binary");
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        out->launcher_args[1] = NULL;
        return 0;
    }

    /* 32-bit little-endian (x86 macOS) */
    if (magic == MACHO_MAGIC_32) {
        out->fmt              = BIN_MACHO;
        out->bits             = 32;
        out->is_little_endian = 1;
        uint32_t cputype = read_le32(&buf[4]);
        if (cputype == CPU_TYPE_X86) {
            out->arch = ARCH_X86;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 32-bit x86 macOS binary");
        } else if (cputype == CPU_TYPE_ARM) {
            out->arch = ARCH_ARM;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 32-bit ARM macOS binary");
        } else {
            out->arch = ARCH_UNKNOWN;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 32-bit macOS binary (cpu=0x%08x)", cputype);
        }
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        out->launcher_args[1] = NULL;
        return 0;
    }

    /* 32-bit big-endian (PowerPC macOS) */
    if (magic == MACHO_CIGAM_32) {
        out->fmt              = BIN_MACHO;
        out->bits             = 32;
        out->is_little_endian = 0;
        uint32_t cputype      = read_be32(&buf[4]);
        out->arch             = ARCH_UNKNOWN;
        if (cputype == CPU_TYPE_POWERPC) {
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 32-bit PowerPC macOS binary");
        } else {
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 32-bit big-endian macOS binary (cpu=0x%08x)", cputype);
        }
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        out->launcher_args[1] = NULL;
        return 0;
    }

    /* 64-bit little-endian (x86-64 / ARM64 macOS) */
    if (magic == MACHO_MAGIC_64) {
        out->fmt              = BIN_MACHO;
        out->bits             = 64;
        out->is_little_endian = 1;
        uint32_t cputype = read_le32(&buf[4]);
        if (cputype == CPU_TYPE_X86_64) {
            out->arch = ARCH_X86_64;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 64-bit x86-64 macOS binary");
        } else if (cputype == CPU_TYPE_ARM64) {
            out->arch = ARCH_ARM64;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 64-bit ARM64 macOS binary");
        } else {
            out->arch = ARCH_UNKNOWN;
            snprintf(out->description, sizeof(out->description),
                     "Mach-O 64-bit macOS binary (cpu=0x%08x)", cputype);
        }
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        out->launcher_args[1] = NULL;
        return 0;
    }

    /* 64-bit big-endian */
    if (magic == MACHO_CIGAM_64) {
        out->fmt              = BIN_MACHO;
        out->bits             = 64;
        out->is_little_endian = 0;
        out->arch             = ARCH_UNKNOWN;
        snprintf(out->description, sizeof(out->description),
                 "Mach-O 64-bit big-endian macOS binary");
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        out->launcher_args[1] = NULL;
        return 0;
    }

    return -1;
}

/* ── Public API ────────────────────────────────────────────────────── */

int fusion_detect_format(const char *path, FusionBinInfo *out) {
    if (!path || !out) return -1;

    out->fmt              = BIN_UNKNOWN;
    out->arch             = ARCH_UNKNOWN;
    out->bits             = 0;
    out->is_little_endian = 0;
    out->launcher         = NULL;
    out->launcher_args[0] = NULL;
    out->launcher_args[1] = NULL;
    out->launcher_args[2] = NULL;
    out->launcher_args[3] = NULL;
    out->description[0]   = '\0';

    if (detect_elf(path, out)   == 0) return 0;
    if (detect_pe(path, out)    == 0) return 0;
    if (detect_macho(path, out) == 0) return 0;

    return -1;
}
