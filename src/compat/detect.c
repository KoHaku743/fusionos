#include "detect.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <endian.h>

/* ELF header constants */
#define EI_NIDENT 16
#define EM_386        3
#define EM_X86_64    62
#define EM_ARM       40
#define EM_AARCH64  183
#define EM_RISCV   243

/* PE/COFF constants */
#define PE_SIGNATURE 0x4550  /* "PE\0\0" */
#define COFF_ARM64  0xaa64

/* Mach-O constants */
#define FAT_MAGIC    0xcafebabe
#define FAT_CIGAM    0xbebafeca
#define MACHO_32     0xfeedface
#define MACHO_64     0xfeedfacf
#define MACHO_32_REV 0xcefaedfe
#define MACHO_64_REV 0xcffaedfe

#define CPU_TYPE_I386    7
#define CPU_TYPE_X86_64  7
#define CPU_TYPE_ARM    12
#define CPU_TYPE_ARM64  0x0100000c

/* Helper: read bytes from file */
static int read_bytes(int fd, off_t offset, void *buf, size_t count) {
    if (lseek(fd, offset, SEEK_SET) == -1) return -1;
    
    ssize_t n = 0;
    size_t remaining = count;
    char *ptr = (char *) buf;
    
    while (remaining > 0) {
        ssize_t ret = read(fd, ptr, remaining);
        if (ret == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;  /* EOF before expected bytes */
        n += ret;
        ptr += ret;
        remaining -= ret;
    }
    
    if (errno == EINTR) return read_bytes(fd, offset, buf, count);
    return n;
}

/**
 * Detect ELF format:
 * - Read ELF header (64 bytes)
 * - Extract e_machine field (offset 18, 2 bytes, little-endian in file)
 * - Extract e_ident for arch info
 */
int detect_elf(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    
    unsigned char elf_hdr[64];
    if (read_bytes(fd, 0, elf_hdr, 64) != 64) {
        close(fd);
        return -1;
    }
    close(fd);
    
    /* Verify ELF magic */
    if (elf_hdr[0] != 0x7f || elf_hdr[1] != 'E' || 
        elf_hdr[2] != 'L' || elf_hdr[3] != 'F') {
        return -1;
    }
    
    /* Get class (32 or 64-bit) and endianness */
    int bits = (elf_hdr[4] == 1) ? 32 : 64;
    int is_little = (elf_hdr[5] == 1);
    
    /* Read e_machine (offset 18, 2 bytes) */
    uint16_t e_machine = *(uint16_t *)(elf_hdr + 18);
    if (!is_little) {
        e_machine = __builtin_bswap16(e_machine);
    }
    
    /* Map machine type to architecture */
    out->bits = bits;
    out->is_little_endian = is_little;
    out->fmt = BIN_ELF;
    
    switch (e_machine) {
        case EM_386:
            out->arch = ARCH_X86;
            out->launcher = "/usr/bin/fusion-run";
            out->launcher_args[0] = "fusion-run";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF 32-bit x86 (i386)");
            break;
        case EM_X86_64:
            out->arch = ARCH_X86_64;
            out->launcher = "/usr/bin/fusion-run";
            out->launcher_args[0] = "fusion-run";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF 64-bit x86-64");
            break;
        case EM_ARM:
            out->arch = ARCH_ARM;
            out->bits = 32;
            out->launcher = "/usr/bin/box64";
            out->launcher_args[0] = "box64";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF 32-bit ARM (ARMv7)");
            break;
        case EM_AARCH64:
            out->arch = ARCH_ARM64;
            out->bits = 64;
            out->launcher = "/usr/bin/fusion-run";
            out->launcher_args[0] = "fusion-run";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF 64-bit ARM (AArch64)");
            break;
        case EM_RISCV:
            out->arch = ARCH_RISCV;
            out->launcher = "/usr/bin/fusion-run";
            out->launcher_args[0] = "fusion-run";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF RISC-V %d-bit", bits);
            break;
        default:
            out->arch = ARCH_UNKNOWN;
            out->launcher = NULL;
            snprintf(out->description, sizeof(out->description),
                    "ELF unknown machine type 0x%04x", e_machine);
            return -1;
    }
    
    return 0;
}

/**
 * Detect PE format:
 * - Read DOS header (64 bytes) to get PE offset
 * - Read COFF header at PE offset
 * - Extract Machine field to determine architecture
 */
int detect_pe(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    
    /* Read DOS header */
    unsigned char dos_hdr[64];
    if (read_bytes(fd, 0, dos_hdr, 64) != 64) {
        close(fd);
        return -1;
    }
    
    /* Verify MZ signature */
    if (dos_hdr[0] != 'M' || dos_hdr[1] != 'Z') {
        close(fd);
        return -1;
    }
    
    /* Get PE offset (at 0x3c, 4 bytes, little-endian) */
    uint32_t pe_offset = *(uint32_t *)(dos_hdr + 0x3c);
    
    /* Read PE signature and COFF header */
    unsigned char pe_hdr[24];
    if (read_bytes(fd, pe_offset, pe_hdr, 24) != 24) {
        close(fd);
        return -1;
    }
    close(fd);
    
    /* Verify PE signature */
    uint32_t sig = *(uint32_t *)pe_hdr;
    if (sig != PE_SIGNATURE) {
        return -1;
    }
    
    /* Machine type is at offset 4 in COFF header */
    uint16_t machine = *(uint16_t *)(pe_hdr + 4);
    
    /* Get optional header magic to determine 32 vs 64-bit (offset 20) */
    uint16_t opt_magic = *(uint16_t *)(pe_hdr + 20);
    int bits = 0;
    if (opt_magic == 0x010b) bits = 32;   /* PE32 */
    else if (opt_magic == 0x020b) bits = 64;  /* PE32+ */
    
    out->fmt = BIN_PE;
    out->is_little_endian = 1;  /* PE is always little-endian */
    out->bits = bits;
    
    /* Map machine type */
    switch (machine) {
        case 0x014C:  /* i386 */
            out->arch = ARCH_X86;
            out->bits = 32;
            out->launcher = "/usr/bin/wine";
            out->launcher_args[0] = "wine";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "PE 32-bit x86 Windows executable");
            break;
        case 0x8664:  /* x86-64 */
            out->arch = ARCH_X86_64;
            out->bits = 64;
            out->launcher = "/usr/bin/wine64";
            out->launcher_args[0] = "wine64";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "PE 64-bit x86-64 Windows executable");
            break;
        case 0xAA64:  /* ARM64 */
            out->arch = ARCH_ARM64;
            out->bits = 64;
            out->launcher = "/usr/bin/fusion-run";
            out->launcher_args[0] = "fusion-run";
            out->launcher_args[1] = NULL;
            snprintf(out->description, sizeof(out->description),
                    "PE 64-bit ARM64 Windows executable");
            break;
        default:
            out->arch = ARCH_UNKNOWN;
            out->launcher = NULL;
            snprintf(out->description, sizeof(out->description),
                    "PE unknown machine type 0x%04x", machine);
            return -1;
    }
    
    return 0;
}

/**
 * Detect Mach-O format:
 * - Read first 4 bytes to check for FAT header
 * - If FAT: read FAT header and enumerate architectures
 * - If regular Mach-O: read common architecture fields
 */
int detect_macho(const char *path, FusionBinInfo *out) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    
    uint32_t magic;
    if (read_bytes(fd, 0, &magic, 4) != 4) {
        close(fd);
        return -1;
    }
    
    /* Check for FAT/Universal binary */
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        /* FAT binary - contains multiple architectures */
        unsigned char fat_hdr[8];
        if (read_bytes(fd, 0, fat_hdr, 8) != 8) {
            close(fd);
            return -1;
        }
        close(fd);
        
        uint32_t nfat_arch = *(uint32_t *)(fat_hdr + 4);
        if (magic == FAT_CIGAM) {
            nfat_arch = __builtin_bswap32(nfat_arch);
        }
        
        out->fmt = BIN_MACHO_FAT;
        out->arch = ARCH_UNKNOWN;  /* Multiple architectures */
        snprintf(out->description, sizeof(out->description),
                "Mach-O Universal FAT binary with %u architectures", nfat_arch);
        out->launcher = "/usr/bin/fusion-run";
        out->launcher_args[0] = "fusion-run";
        out->launcher_args[1] = NULL;
        return 0;
    }
    
    /* Regular Mach-O */
    int is_64bit = 0;
    int is_little = 1;
    uint32_t cputype;
    
    if (magic == MACHO_32) {
        is_64bit = 0;
        is_little = 1;
    } else if (magic == MACHO_64) {
        is_64bit = 1;
        is_little = 1;
    } else if (magic == MACHO_32_REV) {
        is_64bit = 0;
        is_little = 0;
    } else if (magic == MACHO_64_REV) {
        is_64bit = 1;
        is_little = 0;
    } else {
        close(fd);
        return -1;
    }
    
    /* Read CPU type (offset 4, 4 bytes) */
    if (read_bytes(fd, 4, &cputype, 4) != 4) {
        close(fd);
        return -1;
    }
    close(fd);
    
    if (!is_little) {
        cputype = __builtin_bswap32(cputype);
    }
    
    out->fmt = BIN_MACHO;
    out->bits = is_64bit ? 64 : 32;
    out->is_little_endian = is_little;
    out->launcher = "/usr/bin/darling";
    out->launcher_args[0] = "darling";
    out->launcher_args[1] = NULL;
    
    /* Map CPU type */
    switch (cputype & 0xFFFFFF) {  /* Mask off CPU_SUBTYPE bits */
        case CPU_TYPE_I386:
            out->arch = ARCH_X86;
            snprintf(out->description, sizeof(out->description),
                    "Mach-O 32-bit i386 macOS binary");
            break;
        case CPU_TYPE_X86_64:
            out->arch = ARCH_X86_64;
            snprintf(out->description, sizeof(out->description),
                    "Mach-O 64-bit x86-64 macOS binary");
            break;
        case CPU_TYPE_ARM:
            out->arch = ARCH_ARM;
            snprintf(out->description, sizeof(out->description),
                    "Mach-O 32-bit ARM macOS binary");
            break;
        case CPU_TYPE_ARM64:
            out->arch = ARCH_ARM64;
            snprintf(out->description, sizeof(out->description),
                    "Mach-O 64-bit ARM64 macOS binary");
            break;
        default:
            out->arch = ARCH_UNKNOWN;
            snprintf(out->description, sizeof(out->description),
                    "Mach-O unknown CPU type 0x%08x", cputype);
            return -1;
    }
    
    return 0;
}

/**
 * Main detection function: determine binary format and properties
 */
int fusion_detect_format(const char *path, FusionBinInfo *out) {
    if (!path || !out) return -1;
    
    memset(out, 0, sizeof(FusionBinInfo));
    
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    
    /* Read first 4 bytes to identify format */
    unsigned char magic[4];
    ssize_t ret = read(fd, magic, 4);
    if (ret != 4) {
        close(fd);
        return -1;
    }
    close(fd);
    
    /* Try each format detector */
    if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        return detect_elf(path, out);
    }
    
    if (magic[0] == 'M' && magic[1] == 'Z') {
        return detect_pe(path, out);
    }
    
    /* Mach-O checks */
    uint32_t mach_magic = *(uint32_t *)magic;
    if (mach_magic == FAT_MAGIC || mach_magic == FAT_CIGAM ||
        mach_magic == MACHO_32 || mach_magic == MACHO_64 ||
        mach_magic == MACHO_32_REV || mach_magic == MACHO_64_REV) {
        return detect_macho(path, out);
    }
    
    /* Unknown format */
    out->fmt = BIN_UNKNOWN;
    out->arch = ARCH_UNKNOWN;
    snprintf(out->description, sizeof(out->description),
            "Unknown binary format (magic: %02x %02x %02x %02x)",
            magic[0], magic[1], magic[2], magic[3]);
    return -1;
}
