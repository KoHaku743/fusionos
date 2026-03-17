#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_RESET   "\x1b[0m"

#define PKG_DB_PATH "/var/lib/fusion-pkg/packages.json"

typedef enum {
    PKG_TYPE_APT,
    PKG_TYPE_SOURCE,
    PKG_TYPE_NATIVE,
} PkgType;

typedef struct {
    char name[64];
    char version[32];
    PkgType type;
    char apt_packages[256];
    char url[256];
    char compat[32];
    time_t install_time;
    int installed;
} Package;

/* Hardcoded package registry */
static Package registry[] = {
    {
        .name = "wine",
        .version = "8.0",
        .type = PKG_TYPE_APT,
        .apt_packages = "wine wine64 winetricks",
        .compat = "wine",
        .installed = 0,
        .install_time = 0
    },
    {
        .name = "box64",
        .version = "0.2.0",
        .type = PKG_TYPE_APT,
        .apt_packages = "box64",
        .compat = "box64",
        .installed = 0,
        .install_time = 0
    },
    {
        .name = "fex",
        .version = "0.13",
        .type = PKG_TYPE_APT,
        .apt_packages = "fex-emu",
        .compat = "fex",
        .installed = 0,
        .install_time = 0
    },
    {
        .name = "darling",
        .version = "0.1.1",
        .type = PKG_TYPE_SOURCE,
        .url = "https://github.com/darlinghq/darling",
        .compat = "darling",
        .installed = 0,
        .install_time = 0
    },
    {
        .name = "chromium",
        .version = "latest",
        .type = PKG_TYPE_APT,
        .apt_packages = "chromium",
        .compat = "native",
        .installed = 0,
        .install_time = 0
    },
    {
        .name = "firefox",
        .version = "latest",
        .type = PKG_TYPE_APT,
        .apt_packages = "firefox",
        .compat = "native",
        .installed = 0,
        .install_time = 0
    },
    { .name = "", .version = "" }  /* Sentinel */
};

static int registry_size = 6;

/**
 * Find package in registry
 */
static Package *find_package(const char *name) {
    for (int i = 0; i < registry_size; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            return &registry[i];
        }
    }
    return NULL;
}

/**
 * Colorized output helpers
 */
static void print_status(const char *msg, const char *color) {
    printf("%s[*]%s %s\n", color, ANSI_RESET, msg);
}

static void print_success(const char *msg) {
    printf("%s[✓]%s %s\n", ANSI_GREEN, ANSI_RESET, msg);
}

static void print_error(const char *msg) {
    printf("%s[✗]%s %s\n", ANSI_RED, ANSI_RESET, msg);
}

static void print_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("%s[i]%s ", ANSI_BLUE, ANSI_RESET);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

/**
 * Load package database from JSON
 */
static int load_database(void) {
    int fd = open(PKG_DB_PATH, O_RDONLY);
    if (fd == -1) {
        if (errno != ENOENT) {
            perror("open");
        }
        return -1;
    }
    
    /* TODO: Parse JSON database file */
    /* For now, just use registry */
    close(fd);
    return 0;
}

/**
 * Save package database to JSON
 */
static int save_database(void) {
    /* Create directory if needed */
    char dir[256];
    strncpy(dir, PKG_DB_PATH, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    
    int fd = open(PKG_DB_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("open");
        return -1;
    }
    
    /* Write minimal JSON database */
    dprintf(fd, "{\n  \"packages\": [\n");
    
    for (int i = 0; i < registry_size; i++) {
        if (registry[i].name[0] == '\0') break;
        
        dprintf(fd, "    {\n");
        dprintf(fd, "      \"name\": \"%s\",\n", registry[i].name);
        dprintf(fd, "      \"version\": \"%s\",\n", registry[i].version);
        dprintf(fd, "      \"type\": \"%s\",\n",
                registry[i].type == PKG_TYPE_APT ? "apt" :
                registry[i].type == PKG_TYPE_SOURCE ? "source" : "native");
        dprintf(fd, "      \"installed\": %s,\n",
                registry[i].installed ? "true" : "false");
        dprintf(fd, "      \"install_time\": %ld\n", registry[i].install_time);
        dprintf(fd, "    }%s\n", i < registry_size - 1 ? "," : "");
    }
    
    dprintf(fd, "  ]\n}\n");
    close(fd);
    return 0;
}

/**
 * Install a package
 */
static int cmd_install(const char *pkg_name) {
    Package *pkg = find_package(pkg_name);
    if (!pkg) {
        print_error("Package not found in registry");
        return 1;
    }
    
    if (pkg->installed) {
        print_info("%s is already installed", pkg->name);
        return 0;
    }
    
    printf("%s=== Installing %s ===%s\n", ANSI_YELLOW, pkg->name, ANSI_RESET);
    
    switch (pkg->type) {
        case PKG_TYPE_APT:
            print_status("Updating package list", ANSI_BLUE);
            if (system("apt-get update -qq") != 0) {
                print_error("Package list update failed (continuing anyway)");
            }
            
            print_status("Installing APT packages", ANSI_BLUE);
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "apt-get install -y -qq %s", pkg->apt_packages);
            if (system(cmd) != 0) {
                print_error("Failed to install package");
                return 1;
            }
            
            pkg->installed = 1;
            pkg->install_time = time(NULL);
            print_success("Package installed");
            break;
            
        case PKG_TYPE_SOURCE:
            print_info("Building from source: %s", pkg->url);
            print_status("Clone and build instructions:", ANSI_YELLOW);
            printf("  git clone %s\n", pkg->url);
            printf("  cd %s && mkdir build && cd build\n", pkg->name);
            printf("  cmake .. && make && sudo make install\n");
            print_info("Manual installation required for source packages");
            break;
            
        case PKG_TYPE_NATIVE:
            print_info("Native FusionOS package - available natively");
            break;
    }
    
    save_database();
    return 0;
}

/**
 * Remove a package
 */
static int cmd_remove(const char *pkg_name) {
    Package *pkg = find_package(pkg_name);
    if (!pkg) {
        print_error("Package not found");
        return 1;
    }
    
    if (!pkg->installed) {
        print_info("%s is not installed", pkg->name);
        return 0;
    }
    
    printf("%s=== Removing %s ===%s\n", ANSI_YELLOW, pkg->name, ANSI_RESET);
    
    if (pkg->type == PKG_TYPE_APT) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "apt-get remove -y -qq %s", pkg->apt_packages);
        if (system(cmd) != 0) {
            print_error("Failed to remove package");
            return 1;
        }
    }
    
    pkg->installed = 0;
    pkg->install_time = 0;
    print_success("Package removed");
    save_database();
    return 0;
}

/**
 * List all packages
 */
static int cmd_list(void) {
    printf("%s=== FusionOS Packages ===%s\n\n", ANSI_BLUE, ANSI_RESET);
    printf("Name      Version  Type     Compat     Status\n");
    printf("---       -------- ----     ------     ------\n");
    
    for (int i = 0; i < registry_size; i++) {
        if (registry[i].name[0] == '\0') break;
        
        const char *status_color = registry[i].installed ? ANSI_GREEN : ANSI_RED;
        const char *status_text = registry[i].installed ? "installed" : "available";
        const char *type_text =
            registry[i].type == PKG_TYPE_APT ? "apt" :
            registry[i].type == PKG_TYPE_SOURCE ? "source" : "native";
        
        printf("%-9s %-8s %-8s %-10s %s%s%s\n",
               registry[i].name,
               registry[i].version,
               type_text,
               registry[i].compat,
               status_color, status_text, ANSI_RESET);
    }
    
    return 0;
}

/**
 * Search for packages
 */
static int cmd_search(const char *term) {
    int found = 0;
    printf("%s=== Search Results ===%s\n\n", ANSI_BLUE, ANSI_RESET);
    
    for (int i = 0; i < registry_size; i++) {
        if (registry[i].name[0] == '\0') break;
        
        if (strstr(registry[i].name, term) || strstr(registry[i].compat, term)) {
            printf("%s • %s%s v%s - %s\n",
                   ANSI_GREEN, registry[i].name, ANSI_RESET,
                   registry[i].version, registry[i].compat);
            found++;
        }
    }
    
    if (!found) {
        print_info("No packages found matching '%s'", term);
    }
    
    return 0;
}

/**
 * Show package info
 */
static int cmd_info(const char *pkg_name) {
    Package *pkg = find_package(pkg_name);
    if (!pkg) {
        print_error("Package not found");
        return 1;
    }
    
    printf("%s=== Package Info ===%s\n\n", ANSI_BLUE, ANSI_RESET);
    printf("Name:     %s\n", pkg->name);
    printf("Version:  %s\n", pkg->version);
    printf("Type:     %s\n",
           pkg->type == PKG_TYPE_APT ? "APT" :
           pkg->type == PKG_TYPE_SOURCE ? "Source" : "Native");
    printf("Compat:   %s\n", pkg->compat);
    printf("Status:   %s%s%s\n",
           pkg->installed ? ANSI_GREEN : ANSI_RED,
           pkg->installed ? "installed" : "not installed",
           ANSI_RESET);
    
    if (pkg->installed && pkg->install_time) {
        printf("Installed: %s", ctime(&pkg->install_time));
    }
    
    if (pkg->type == PKG_TYPE_APT && pkg->apt_packages[0]) {
        printf("APT Packages: %s\n", pkg->apt_packages);
    } else if (pkg->type == PKG_TYPE_SOURCE && pkg->url[0]) {
        printf("Repository: %s\n", pkg->url);
    }
    
    return 0;
}

/**
 * Update package cache
 */
static int cmd_update(void) {
    print_status("Updating package cache", ANSI_BLUE);
    if (system("apt-get update -qq") != 0) {
        print_error("Package cache update failed");
        return 1;
    }
    print_success("Cache updated");
    return 0;
}

/**
 * Print help
 */
static void print_help(const char *prog) {
    printf("FusionOS Package Manager v1.0\n\n");
    printf("Usage: %s <command> [args]\n\n", prog);
    printf("Commands:\n");
    printf("  install <pkg>   - Install a package\n");
    printf("  remove <pkg>    - Remove an installed package\n");
    printf("  list            - List all available packages\n");
    printf("  search <term>   - Search for packages\n");
    printf("  info <pkg>      - Show detailed package information\n");
    printf("  update          - Update package cache\n");
    printf("  help            - Show this help message\n");
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }
    
    load_database();
    
    const char *cmd = argv[1];
    
    if (strcmp(cmd, "help") == 0) {
        print_help(argv[0]);
        return 0;
    }
    
    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            print_error("Missing package name");
            return 1;
        }
        return cmd_install(argv[2]);
    }
    
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            print_error("Missing package name");
            return 1;
        }
        return cmd_remove(argv[2]);
    }
    
    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }
    
    if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            print_error("Missing search term");
            return 1;
        }
        return cmd_search(argv[2]);
    }
    
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            print_error("Missing package name");
            return 1;
        }
        return cmd_info(argv[2]);
    }
    
    if (strcmp(cmd, "update") == 0) {
        return cmd_update();
    }
    
    printf("Unknown command: %s\n", cmd);
    print_help(argv[0]);
    return 1;
}
