#include "../compat/detect.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/**
 * fusion-run: Universal launcher for cross-platform binaries
 * - Detects ELF/PE/Mach-O binary format
 * - Routes to Wine, Darling, Box64, or native execution
 * - Transparent from the command line
 */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <program> [arguments...]\n", prog);
    fprintf(stderr, "fusion-run is a universal binary launcher.\n");
}

static void print_detection_error(const char *prog) {
    fprintf(stderr, "fusion-run: could not detect format for '%s'\n", prog);
}

/**
 * Main launcher logic
 */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *prog = argv[1];
    FusionBinInfo info;
    
    /* Detect binary format */
    if (fusion_detect_format(prog, &info) != 0) {
        print_detection_error(prog);
        return 127;
    }
    
    /* Native ELF binaries run directly */
    if (info.fmt == BIN_ELF && (info.arch == ARCH_X86_64 || info.arch == ARCH_X86)) {
        execv(prog, argv + 1);
        perror("execv");
        return 127;
    }
    
    /* Build launcher command */
    const char *launcher = NULL;
    const char **launcher_base_args = NULL;
    
    if (!info.launcher) {
        fprintf(stderr, "fusion-run: no launcher available for %s\n", info.description);
        return 127;
    }
    
    launcher = info.launcher;
    launcher_base_args = (const char **) info.launcher_args;
    
    /* Build new arguments array: launcher [launcher_args] prog [original_args] */
    char **new_argv = NULL;
    int new_argc = 0;
    
    /* Count launcher args */
    int launcher_arg_count = 0;
    if (launcher_base_args) {
        while (launcher_base_args[launcher_arg_count]) {
            launcher_arg_count++;
        }
    }
    
    /* Allocate new argv: launcher + launcher_args + prog + original_args + NULL */
    new_argc = 1 + launcher_arg_count + (argc - 1) + 1;
    new_argv = malloc(new_argc * sizeof(char *));
    if (!new_argv) {
        perror("malloc");
        return 127;
    }
    
    int idx = 0;
    new_argv[idx++] = (char *) launcher;
    
    /* Add launcher args (skip first one if it's just the launcher name) */
    if (launcher_base_args) {
        for (int i = 0; i < launcher_arg_count; i++) {
            if (strcmp(launcher_base_args[i], launcher) != 0) {
                new_argv[idx++] = (char *) launcher_base_args[i];
            }
        }
    }
    
    /* Add program and its arguments */
    for (int i = 1; i < argc; i++) {
        new_argv[idx++] = argv[i];
    }
    new_argv[idx] = NULL;
    
    /* Print debug info if verbose */
    if (getenv("FUSION_DEBUG")) {
        fprintf(stderr, "fusion-run: detected %s\n", info.description);
        fprintf(stderr, "fusion-run: launching '%s'", launcher);
        for (int i = 1; new_argv[i]; i++) {
            fprintf(stderr, " '%s'", new_argv[i]);
        }
        fprintf(stderr, "\n");
    }
    
    /* Execute launcher with program */
    execv(launcher, new_argv);
    
    /* If exec fails */
    fprintf(stderr, "fusion-run: could not execute launcher '%s': %s\n", 
            launcher, strerror(errno));
    free(new_argv);
    return 127;
}
