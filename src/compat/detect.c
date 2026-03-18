#include "detect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Magic numbers and format constants
 * -------------------------------------------------------------------- */

/* ELF */
#define ELF_MAGIC_0    0x7f
#define ELF_MAGIC_1    'E'
#define ELF_MAGIC_2    'L'
#define ELF_MAGIC_3    'F'
#define ELFCLASS32     1
#define ELFCLASS64     2
#define ELFDATA2LSB    1   /* little-endian */
#define ELFDATA2MSB    2   /* big-endian    */
#define EM_386         3
#define EM_ARM         40
#define EM_X86_64      62
#define EM_AARCH64     183
#define EM_RISCV       243

/* PE/COFF */
#define PE_MACHINE_I386   0x014c
#define PE_MACHINE_X64    0x8664
#define PE_MACHINE_ARM    0x01c4
#define PE_MACHINE_ARM64  0xaa64
#define PE_OPT_PE32       0x010b
#define PE_OPT_PE32PLUS   0x020b

/* Maximum sane byte offset for the PE header within the file */
#define PE_MAX_HEADER_OFFSET 0x10000u
#define MACHO_MH_MAGIC       0xFEEDFACEU  /* 32-bit big-endian    */
#define MACHO_MH_CIGAM       0xCEFAEDFEU  /* 32-bit little-endian */
#define MACHO_MH_MAGIC_64    0xFEEDFACFU  /* 64-bit big-endian    */
#define MACHO_MH_CIGAM_64    0xCFFAEDFEU  /* 64-bit little-endian */
#define MACHO_FAT_MAGIC      0xCAFEBABEU  /* FAT big-endian       */
#define MACHO_FAT_CIGAM      0xBEBAFECAU  /* FAT little-endian    */

/* Mach-O cpu_type_t */
#define MACHO_CPU_X86       7
#define MACHO_CPU_X86_64    0x01000007U
#define MACHO_CPU_ARM       12
#define MACHO_CPU_ARM64     0x0100000cU

/* -----------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------- */

/* Read exactly `count` bytes starting at byte offset `off` from `path`. */
static int read_bytes(const char *path, uint8_t *buf, size_t count, long off)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (off > 0 && fseek(f, off, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    size_t got = fread(buf, 1, count, f);
    fclose(f);
    return (got == count) ? 0 : -1;
}

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8)
                   | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint32_t u32be(const uint8_t *p)
{
    return (uint32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                   |  ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}

/* -----------------------------------------------------------------------
 * ELF detection
 * -------------------------------------------------------------------- */
int detect_elf(const char *path, FusionBinInfo *out)
{
    uint8_t hdr[64];
    if (read_bytes(path, hdr, sizeof(hdr), 0) != 0) return -1;

    if (hdr[0] != ELF_MAGIC_0 || hdr[1] != ELF_MAGIC_1 ||
        hdr[2] != ELF_MAGIC_2 || hdr[3] != ELF_MAGIC_3) return -1;

    out->fmt              = BIN_ELF;
    out->bits             = (hdr[4] == ELFCLASS64) ? 64 : 32;
    out->is_little_endian = (hdr[5] == ELFDATA2LSB) ? 1 : 0;

    /* e_machine is at offset 18 (2 bytes) */
    uint16_t machine = out->is_little_endian
                       ? u16le(hdr + 18)
                       : (uint16_t)((hdr[18] << 8) | hdr[19]);

    out->launcher_args[0] = NULL;
    out->launcher_args[1] = NULL;
    out->launcher_args[2] = NULL;
    out->launcher_args[3] = NULL;

    switch (machine) {
    case EM_386:
        /* Runs natively on x86-64 kernels with CONFIG_IA32_EMULATION=y */
        out->arch    = ARCH_X86;
        out->launcher = NULL;
        snprintf(out->description, sizeof(out->description),
                 "ELF 32-bit x86 (i386)");
        break;
    case EM_X86_64:
        out->arch    = ARCH_X86_64;
        out->launcher = NULL;
        snprintf(out->description, sizeof(out->description),
                 "ELF 64-bit x86-64");
        break;
    case EM_ARM:
        out->arch             = ARCH_ARM;
        out->launcher         = "/usr/bin/box64";
        out->launcher_args[0] = "/usr/bin/box64";
        snprintf(out->description, sizeof(out->description),
                 "ELF 32-bit ARM");
        break;
    case EM_AARCH64:
        out->arch             = ARCH_ARM64;
        out->launcher         = "/usr/bin/fex-emu";
        out->launcher_args[0] = "/usr/bin/fex-emu";
        snprintf(out->description, sizeof(out->description),
                 "ELF 64-bit AArch64");
        break;
    case EM_RISCV:
        out->arch    = (out->bits == 64) ? ARCH_RISCV64 : ARCH_RISCV;
        out->launcher = NULL;
        snprintf(out->description, sizeof(out->description),
                 "ELF %d-bit RISC-V", out->bits);
        break;
    default:
        out->arch    = ARCH_UNKNOWN;
        out->launcher = NULL;
        snprintf(out->description, sizeof(out->description),
                 "ELF %d-bit (e_machine=0x%04x)", out->bits, machine);
        break;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * PE / COFF detection
 * -------------------------------------------------------------------- */
int detect_pe(const char *path, FusionBinInfo *out)
{
    /* Read DOS header (64 bytes covers the e_lfanew field at 0x3c) */
    uint8_t dos[64];
    if (read_bytes(path, dos, sizeof(dos), 0) != 0) return -1;
    if (dos[0] != 'M' || dos[1] != 'Z') return -1;

    uint32_t pe_off = u32le(dos + 0x3c);
    if (pe_off < 4 || pe_off > PE_MAX_HEADER_OFFSET) return -1;

    /* Read PE signature (4) + COFF header (20) + optional header magic (2) */
    uint8_t pe[26];
    if (read_bytes(path, pe, sizeof(pe), (long)pe_off) != 0) return -1;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return -1;

    out->fmt              = BIN_PE;
    out->is_little_endian = 1; /* PE is always little-endian */

    uint16_t opt_magic    = u16le(pe + 24);
    out->bits             = (opt_magic == PE_OPT_PE32PLUS) ? 64 : 32;

    uint16_t machine      = u16le(pe + 4);

    out->launcher_args[0] = NULL;
    out->launcher_args[1] = NULL;
    out->launcher_args[2] = NULL;
    out->launcher_args[3] = NULL;

    switch (machine) {
    case PE_MACHINE_I386:
        out->arch             = ARCH_X86;
        out->launcher         = "/usr/bin/wine";
        out->launcher_args[0] = "/usr/bin/wine";
        snprintf(out->description, sizeof(out->description),
                 "PE32 Windows x86 (i386)");
        break;
    case PE_MACHINE_X64:
        out->arch             = ARCH_X86_64;
        out->launcher         = "/usr/bin/wine64";
        out->launcher_args[0] = "/usr/bin/wine64";
        snprintf(out->description, sizeof(out->description),
                 "PE32+ Windows x64");
        break;
    case PE_MACHINE_ARM:
        out->arch             = ARCH_ARM;
        out->launcher         = "/usr/bin/wine";
        out->launcher_args[0] = "/usr/bin/wine";
        snprintf(out->description, sizeof(out->description),
                 "PE32 Windows ARM");
        break;
    case PE_MACHINE_ARM64:
        out->arch             = ARCH_ARM64;
        out->launcher         = "/usr/bin/wine64";
        out->launcher_args[0] = "/usr/bin/wine64";
        snprintf(out->description, sizeof(out->description),
                 "PE32+ Windows ARM64");
        break;
    default:
        out->arch             = ARCH_UNKNOWN;
        out->launcher         = "/usr/bin/wine";
        out->launcher_args[0] = "/usr/bin/wine";
        snprintf(out->description, sizeof(out->description),
                 "PE Windows (machine=0x%04x)", machine);
        break;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Mach-O detection
 * -------------------------------------------------------------------- */
int detect_macho(const char *path, FusionBinInfo *out)
{
    uint8_t buf[12];
    if (read_bytes(path, buf, sizeof(buf), 0) != 0) return -1;

    uint32_t magic_le = u32le(buf);
    uint32_t magic_be = u32be(buf);

    int is_64  = 0;
    int is_le  = 0;
    int is_fat = 0;

    if      (magic_le == MACHO_MH_MAGIC)    { is_64 = 0; is_le = 1; }
    else if (magic_be == MACHO_MH_MAGIC)    { is_64 = 0; is_le = 0; }
    else if (magic_le == MACHO_MH_MAGIC_64) { is_64 = 1; is_le = 1; }
    else if (magic_be == MACHO_MH_MAGIC_64) { is_64 = 1; is_le = 0; }
    else if (magic_be == MACHO_FAT_MAGIC ||
             magic_le == MACHO_FAT_CIGAM)   { is_fat = 1; }
    else return -1;

    out->launcher_args[0] = NULL;
    out->launcher_args[1] = NULL;
    out->launcher_args[2] = NULL;
    out->launcher_args[3] = NULL;

    if (is_fat) {
        out->fmt              = BIN_MACHO_FAT;
        out->arch             = ARCH_UNKNOWN;
        out->bits             = 0;
        out->is_little_endian = 0;
        out->launcher         = "/usr/bin/darling";
        out->launcher_args[0] = "/usr/bin/darling";
        snprintf(out->description, sizeof(out->description),
                 "Mach-O FAT Universal Binary");
        return 0;
    }

    out->fmt              = BIN_MACHO;
    out->bits             = is_64 ? 64 : 32;
    out->is_little_endian = is_le;

    /* cpu_type_t at offset 4 */
    uint32_t cpu = is_le ? u32le(buf + 4) : u32be(buf + 4);

    switch (cpu) {
    case MACHO_CPU_X86:
        out->arch = ARCH_X86;    break;
    case MACHO_CPU_X86_64:
        out->arch = ARCH_X86_64; break;
    case MACHO_CPU_ARM:
        out->arch = ARCH_ARM;    break;
    case MACHO_CPU_ARM64:
        out->arch = ARCH_ARM64;  break;
    default:
        out->arch = ARCH_UNKNOWN; break;
    }

    out->launcher         = "/usr/bin/darling";
    out->launcher_args[0] = "/usr/bin/darling";
    snprintf(out->description, sizeof(out->description),
             "Mach-O %d-bit macOS/Darwin", out->bits);
    return 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------- */
int fusion_detect_format(const char *path, FusionBinInfo *out)
{
    if (!path || !out) return -1;

    memset(out, 0, sizeof(*out));
    out->fmt  = BIN_UNKNOWN;
    out->arch = ARCH_UNKNOWN;

    if (detect_elf(path, out)   == 0) return 0;
    if (detect_pe(path, out)    == 0) return 0;
    if (detect_macho(path, out) == 0) return 0;

    return -1;
}
