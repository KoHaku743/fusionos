#ifndef FUSION_DETECT_H
#define FUSION_DETECT_H

#include <stdint.h>

/* Binary format enumeration */
typedef enum {
    BIN_UNKNOWN = 0,
    BIN_ELF,
    BIN_PE,
    BIN_MACHO,
    BIN_MACHO_FAT,
} BinFormat;

/* Architecture enumeration */
typedef enum {
    ARCH_UNKNOWN = 0,
    ARCH_X86,
    ARCH_X86_64,
    ARCH_ARM,
    ARCH_ARM64,
    ARCH_RISCV,
    ARCH_RISCV64,
} BinArch;

/* Binary info structure */
typedef struct {
    BinFormat fmt;
    BinArch arch;
    int bits;                      /* 32 or 64 */
    int is_little_endian;
    const char *launcher;          /* Recommended launcher path */
    const char *launcher_args[4];  /* Launcher arguments (null-terminated) */
    char description[128];         /* Human-readable description */
} FusionBinInfo;

/**
 * Detect binary format, architecture, and recommend launcher.
 * Opens file, reads magic bytes, parses headers.
 * Returns 0 on success, -1 if file not readable or format unrecognized.
 */
int fusion_detect_format(const char *path, FusionBinInfo *out);

/* Internal helper functions */
int detect_elf(const char *path, FusionBinInfo *out);
int detect_pe(const char *path, FusionBinInfo *out);
int detect_macho(const char *path, FusionBinInfo *out);

#endif /* FUSION_DETECT_H */
