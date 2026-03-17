#define _GNU_SOURCE
#include "../compat/detect.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_CMD_LEN 4096
#define MAX_ARGS 128
#define HISTORY_SIZE 256

/* Command history */
static char history[HISTORY_SIZE][MAX_CMD_LEN];
static int history_count = 0;

/**
 * Print help message
 */
static void cmd_help(void) {
    printf("\nFusionOS Shell (fsh) - Universal Compatibility Shell\n");
    printf("=== Built-in Commands ===\n");
    printf("  help              - Show this help message\n");
    printf("  exit              - Exit the shell\n");
    printf("  cd <dir>          - Change directory\n");
    printf("  pwd               - Print working directory\n");
    printf("  ls [dir]          - List directory\n");
    printf("  history           - Show command history\n");
    printf("  layers            - Show active compat layers\n");
    printf("  compat <file>     - Detect binary format\n");
    printf("  run <prog> [args] - Run program with compat layer\n");
    printf("  install <pkg>     - Install package\n\n");
}

/**
 * List active compat layers
 */
static void cmd_layers(void) {
    printf("Active FusionOS Compatibility Layers:\n");
    printf("  - native ELF x86/x86_64: kernel execution\n");
    printf("  - Wine (PE Windows): x86 Windows binaries\n");
    printf("  - Wine64: x86-64 Windows binaries\n");
    printf("  - Darling (Mach-O macOS): macOS binaries\n");
    printf("  - Box64 (ARM): ARM binaries\n");
    printf("  - FEX (ARM64): ARM64 binaries\n");
}

/**
 * Detect and display binary format
 */
static void cmd_compat(const char *path) {
    FusionBinInfo info;
    
    if (fusion_detect_format(path, &info) == 0) {
        printf("Binary: %s\n", path);
        printf("Format: ");
        switch (info.fmt) {
            case BIN_ELF: printf("ELF"); break;
            case BIN_PE: printf("PE (Windows)"); break;
            case BIN_MACHO: printf("Mach-O (macOS)"); break;
            case BIN_MACHO_FAT: printf("Mach-O Universal"); break;
            default: printf("Unknown"); break;
        }
        printf("\n");
        printf("Architecture: ");
        switch (info.arch) {
            case ARCH_X86: printf("x86"); break;
            case ARCH_X86_64: printf("x86-64"); break;
            case ARCH_ARM: printf("ARM"); break;
            case ARCH_ARM64: printf("ARM64"); break;
            case ARCH_RISCV: printf("RISC-V"); break;
            default: printf("Unknown"); break;
        }
        printf("\n");
        printf("Bits: %d\n", info.bits);
        printf("Launcher: %s\n", info.launcher ? info.launcher : "unknown");
        printf("Description: %s\n", info.description);
    } else {
        printf("Could not detect binary format for: %s\n", path);
    }
}

/**
 * Execute a command, routing through compat layers as needed
 */
static int execute_command(char **args) {
    if (!args || !args[0]) return -1;
    
    const char *prog = args[0];
    
    /* Check if program exists and is executable */
    if (access(prog, X_OK) != 0) {
        /* Try to find in PATH */
        const char *path = getenv("PATH");
        if (!path) path = "/sbin:/bin:/usr/sbin:/usr/bin";
        
        char path_copy[4096];
        strncpy(path_copy, path, sizeof(path_copy) - 1);
        
        char *dir = strtok(path_copy, ":");
        while (dir) {
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, prog);
            
            if (access(full_path, X_OK) == 0) {
                args[0] = full_path;
                break;
            }
            dir = strtok(NULL, ":");
        }
    }
    
    /* Detect binary format to determine launcher */
    FusionBinInfo info;
    int needs_launcher = 0;
    
    if (fusion_detect_format(args[0], &info) == 0) {
        if (info.fmt != BIN_ELF || info.arch != ARCH_X86_64) {
            needs_launcher = 1;
        }
    }
    
    /* Fork and execute */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    
    if (pid == 0) {
        /* Child: execute program */
        if (needs_launcher && fusion_detect_format(args[0], &info) == 0 && info.launcher) {
            /* Use launcher */
            char *launcher_args[MAX_ARGS];
            launcher_args[0] = (char *) info.launcher;
            
            int i = 0;
            while (info.launcher_args[i] && i < MAX_ARGS - 2) {
                launcher_args[i + 1] = (char *) info.launcher_args[i];
                i++;
            }
            
            /* Add original program and arguments */
            launcher_args[i + 1] = args[0];
            i++;
            int j = 1;
            while (args[j] && i < MAX_ARGS - 1) {
                launcher_args[i + 1] = args[j];
                i++;
                j++;
            }
            launcher_args[i + 1] = NULL;
            
            execv(info.launcher, launcher_args);
        }
        
        /* Direct execution or launcher failed - try direct exec */
        execvp(args[0], args);
        fprintf(stderr, "fsh: %s: command not found\n", args[0]);
        _exit(127);
    }
    
    /* Parent: wait for child */
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) break;
    }
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        printf("fsh: command killed by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    
    return -1;
}

/**
 * Parse and execute a command line
 */
static int parse_and_execute(const char *line) {
    if (!line || !*line) return 0;
    
    /* Skip leading whitespace */
    while (*line && (*line == ' ' || *line == '\t')) line++;
    if (!*line) return 0;
    
    /* Duplicate line for parsing */
    char cmd_line[MAX_CMD_LEN];
    strncpy(cmd_line, line, sizeof(cmd_line) - 1);
    
    /* Split into arguments */
    char *args[MAX_ARGS];
    int argc = 0;
    
    char *token = strtok(cmd_line, " \t");
    while (token && argc < MAX_ARGS - 1) {
        args[argc++] = token;
        token = strtok(NULL, " \t");
    }
    args[argc] = NULL;
    
    if (argc == 0) return 0;
    
    /* Built-in commands */
    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }
    
    if (strcmp(args[0], "help") == 0) {
        cmd_help();
        return 0;
    }
    
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            printf("%s\n", cwd);
        }
        return 0;
    }
    
    if (strcmp(args[0], "cd") == 0) {
        if (argc < 2) {
            fprintf(stderr, "cd: missing argument\n");
            return 1;
        }
        if (chdir(args[1]) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }
    
    if (strcmp(args[0], "ls") == 0) {
        args[0] = "ls";
        if (argc == 1) args[1] = ".";
        args[argc + (argc == 1 ? 1 : 0)] = NULL;
        return execute_command(args);
    }
    
    if (strcmp(args[0], "history") == 0) {
        for (int i = 0; i < history_count; i++) {
            printf("%d: %s\n", i + 1, history[i]);
        }
        return 0;
    }
    
    if (strcmp(args[0], "layers") == 0) {
        cmd_layers();
        return 0;
    }
    
    if (strcmp(args[0], "compat") == 0) {
        if (argc < 2) {
            fprintf(stderr, "compat: missing argument\n");
            return 1;
        }
        cmd_compat(args[1]);
        return 0;
    }
    
    if (strcmp(args[0], "run") == 0) {
        if (argc < 2) {
            fprintf(stderr, "run: missing argument\n");
            return 1;
        }
        return execute_command(&args[1]);
    }
    
    if (strcmp(args[0], "install") == 0) {
        if (argc < 2) {
            fprintf(stderr, "install: missing argument\n");
            return 1;
        }
        args[0] = "fusion-pkg";
        return execute_command(args);
    }
    
    /* External command */
    return execute_command(args);
}

/**
 * Read a line from stdin
 */
static int read_line(char *line, size_t size) {
    if (!fgets(line, size, stdin)) {
        return -1;
    }
    
    /* Remove trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }
    
    return 0;
}

/**
 * Main shell loop
 */
int main(void) {
    char line[MAX_CMD_LEN];
    int interactive = isatty(STDIN_FILENO);
    
    if (interactive) {
        printf("FusionOS Shell v1.0 - Type 'help' for commands\n");
    }
    
    while (1) {
        if (interactive) {
            printf("fsh> ");
            fflush(stdout);
        }
        
        if (read_line(line, sizeof(line)) != 0) {
            break;  /* EOF */
        }
        
        /* Add to history */
        if (line[0] && history_count < HISTORY_SIZE) {
            strncpy(history[history_count], line, MAX_CMD_LEN - 1);
            history_count++;
        }
        
        parse_and_execute(line);
    }
    
    return 0;
}
