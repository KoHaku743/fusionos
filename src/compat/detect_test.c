#include "detect.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * detect_test: Test utility for binary detection library
 * Usage: detect_test <binary_path>
 */

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <binary_path>\n", argv[0]);
        fprintf(stderr, "Detect and display information about a binary file.\n");
        return 1;
    }
    
    const char *path = argv[1];
    FusionBinInfo info;
    
    printf("Analyzing: %s\n", path);
    printf("=====================================\n");
    
    if (fusion_detect_format(path, &info) != 0) {
        printf("Status: FAILED - Unknown format\n");
        return 1;
    }
    
    printf("Status: SUCCESS\n");
    printf("Format: ");
    switch (info.fmt) {
        case BIN_ELF:
            printf("ELF (Executable and Linkable Format)\n");
            break;
        case BIN_PE:
            printf("PE (Portable Executable - Windows)\n");
            break;
        case BIN_MACHO:
            printf("Mach-O (macOS/Darwin)\n");
            break;
        case BIN_MACHO_FAT:
            printf("Mach-O FAT Universal Binary\n");
            break;
        case BIN_UNKNOWN:
            printf("Unknown\n");
            break;
    }
    
    printf("Architecture: ");
    switch (info.arch) {
        case ARCH_X86:
            printf("Intel x86 (i386)\n");
            break;
        case ARCH_X86_64:
            printf("Intel x86-64 (x64)\n");
            break;
        case ARCH_ARM:
            printf("ARM (32-bit)\n");
            break;
        case ARCH_ARM64:
            printf("ARM64 (AArch64)\n");
            break;
        case ARCH_RISCV:
            printf("RISC-V\n");
            break;
        case ARCH_RISCV64:
            printf("RISC-V 64-bit\n");
            break;
        case ARCH_UNKNOWN:
            printf("Unknown\n");
            break;
    }
    
    printf("Word Size: %d-bit\n", info.bits);
    printf("Endianness: %s\n", info.is_little_endian ? "Little-endian" : "Big-endian");
    printf("Launcher: %s\n", info.launcher ? info.launcher : "(none)");
    printf("Description: %s\n", info.description);
    printf("=====================================\n");
    
    return 0;
}
