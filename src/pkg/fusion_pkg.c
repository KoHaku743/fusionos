#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

/* ── Configuration ─────────────────────────────────────────────────── */
#define PKG_DB_DIR       "/var/lib/fusion-pkg"
#define PKG_DB_LIST      PKG_DB_DIR "/installed.db"
#define PKG_CACHE_DIR    "/var/cache/fusion-pkg"
#define PKG_REPO_URL     "https://pkg.fusionos.local/repo"   /* placeholder */
#define PKG_NAME_MAX     128
#define PKG_VER_MAX      64
#define PKG_DESC_MAX     256
#define PKG_LINE_MAX     (PKG_NAME_MAX + PKG_VER_MAX + PKG_DESC_MAX + 4)

/* ── Data structures ───────────────────────────────────────────────── */
typedef struct {
    char name[PKG_NAME_MAX];
    char version[PKG_VER_MAX];
    char description[PKG_DESC_MAX];
} Package;

/* ── ANSI colours ──────────────────────────────────────────────────── */
#define CLR_BOLD    "\x1b[1m"
#define CLR_GREEN   "\x1b[32m"
#define CLR_YELLOW  "\x1b[33m"
#define CLR_RED     "\x1b[31m"
#define CLR_CYAN    "\x1b[36m"
#define CLR_RESET   "\x1b[0m"

/* ── Helpers ───────────────────────────────────────────────────────── */

/** Ensure a directory exists (create if needed). */
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/** Run an external command; returns its exit status, or -1 on error. */
static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ── Database helpers ──────────────────────────────────────────────── */

/** Write one package record to the database file (append mode). */
static int db_record_install(const Package *pkg) {
    if (ensure_dir(PKG_DB_DIR) != 0) {
        fprintf(stderr, "fusion-pkg: cannot create database directory %s\n", PKG_DB_DIR);
        return -1;
    }

    FILE *f = fopen(PKG_DB_LIST, "a");
    if (!f) {
        perror("fusion-pkg: open database");
        return -1;
    }
    fprintf(f, "%s\t%s\t%s\n", pkg->name, pkg->version, pkg->description);
    fclose(f);
    return 0;
}

/** Remove a package record from the database (rewrite without it). */
static int db_record_remove(const char *name) {
    FILE *in = fopen(PKG_DB_LIST, "r");
    if (!in) {
        /* Nothing installed — not an error */
        return 0;
    }

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.installed.tmp", PKG_DB_DIR);

    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        perror("fusion-pkg: write temp database");
        return -1;
    }

    char line[PKG_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), in)) {
        char pkg_name[PKG_NAME_MAX];
        if (sscanf(line, "%127s", pkg_name) == 1 && strcmp(pkg_name, name) == 0) {
            found = 1;
            continue;   /* skip this line */
        }
        fputs(line, out);
    }

    fclose(in);
    fclose(out);

    if (rename(tmp_path, PKG_DB_LIST) != 0) {
        perror("fusion-pkg: rename database");
        return -1;
    }

    return found ? 0 : -1;
}

/** Check whether a package is already installed. */
static int db_is_installed(const char *name) {
    FILE *f = fopen(PKG_DB_LIST, "r");
    if (!f) return 0;

    char line[PKG_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char pkg_name[PKG_NAME_MAX];
        if (sscanf(line, "%127s", pkg_name) == 1 &&
            strcmp(pkg_name, name) == 0) {
            found = 1;
            break;
        }
    }

    fclose(f);
    return found;
}

/* ── Subcommand implementations ────────────────────────────────────── */

/** fusion-pkg install <name> */
static int cmd_install(const char *name) {
    if (db_is_installed(name)) {
        printf("%s%s%s is already installed.\n", CLR_GREEN, name, CLR_RESET);
        return 0;
    }

    printf("%s==>%s Installing %s%s%s ...\n",
           CLR_CYAN, CLR_RESET, CLR_BOLD, name, CLR_RESET);

    /*
     * In a real build this would:
     *   1. Query the repo index for package metadata
     *   2. Download the package archive to PKG_CACHE_DIR
     *   3. Verify its checksum
     *   4. Extract it to the correct prefix
     *   5. Run pre/post-install scripts
     *
     * For now we delegate to the system package manager if available,
     * and record the installation in our own database.
     */

    /* Try apt-get, then pacman, then dnf */
    int installed = 0;
    const char *managers[][4] = {
        { "apt-get", "install", "-y", NULL },
        { "pacman",  "-S",      "--noconfirm", NULL },
        { "dnf",     "install", "-y", NULL },
        { NULL, NULL, NULL, NULL },
    };

    for (int i = 0; managers[i][0] != NULL; i++) {
        char *argv[6];
        int j = 0;
        while (managers[i][j]) {
            argv[j] = (char *)managers[i][j];
            j++;
        }
        argv[j++] = (char *)name;
        argv[j]   = NULL;

        int rc = run_cmd(argv);
        if (rc == 0) {
            installed = 1;
            break;
        }
        if (rc == 127) {
            /* command not found, try next manager */
            continue;
        }
        /* non-zero exit from a found manager → package probably not found */
        break;
    }

    if (!installed) {
        fprintf(stderr, "%sfusion-pkg:%s could not install '%s' — "
                "no working package manager found or package unavailable.\n",
                CLR_RED, CLR_RESET, name);
        return 1;
    }

    /* Record in our database */
    Package pkg;
    strncpy(pkg.name,        name,          sizeof(pkg.name) - 1);
    strncpy(pkg.version,     "unknown",     sizeof(pkg.version) - 1);
    strncpy(pkg.description, "installed",   sizeof(pkg.description) - 1);
    pkg.name[sizeof(pkg.name) - 1]               = '\0';
    pkg.version[sizeof(pkg.version) - 1]         = '\0';
    pkg.description[sizeof(pkg.description) - 1] = '\0';

    db_record_install(&pkg);

    printf("%s==>%s Successfully installed %s%s%s\n",
           CLR_GREEN, CLR_RESET, CLR_BOLD, name, CLR_RESET);
    return 0;
}

/** fusion-pkg remove <name> */
static int cmd_remove(const char *name) {
    if (!db_is_installed(name)) {
        fprintf(stderr, "fusion-pkg: '%s' is not installed.\n", name);
        return 1;
    }

    printf("%s==>%s Removing %s%s%s ...\n",
           CLR_CYAN, CLR_RESET, CLR_BOLD, name, CLR_RESET);

    const char *managers[][4] = {
        { "apt-get", "remove", "-y", NULL },
        { "pacman",  "-R",     "--noconfirm", NULL },
        { "dnf",     "remove", "-y", NULL },
        { NULL, NULL, NULL, NULL },
    };

    for (int i = 0; managers[i][0] != NULL; i++) {
        char *argv[6];
        int j = 0;
        while (managers[i][j]) {
            argv[j] = (char *)managers[i][j];
            j++;
        }
        argv[j++] = (char *)name;
        argv[j]   = NULL;

        int rc = run_cmd(argv);
        if (rc == 0) break;
        if (rc == 127) continue;
    }

    if (db_record_remove(name) == 0) {
        printf("%s==>%s Removed %s%s%s\n",
               CLR_GREEN, CLR_RESET, CLR_BOLD, name, CLR_RESET);
        return 0;
    }

    fprintf(stderr, "fusion-pkg: failed to remove '%s' from database.\n", name);
    return 1;
}

/** fusion-pkg list */
static int cmd_list(void) {
    FILE *f = fopen(PKG_DB_LIST, "r");
    if (!f) {
        printf("No packages installed (database not found).\n");
        return 0;
    }

    printf("%s%-30s %-20s %s%s\n",
           CLR_BOLD, "Package", "Version", "Description", CLR_RESET);
    printf("%-30s %-20s %s\n",
           "-------", "-------", "-----------");

    char line[PKG_LINE_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[PKG_NAME_MAX], ver[PKG_VER_MAX], desc[PKG_DESC_MAX];
        name[0] = ver[0] = desc[0] = '\0';

        /* Tab-separated: name<TAB>version<TAB>description */
        char *p = line;
        char *tab;

        tab = strchr(p, '\t');
        if (tab) {
            size_t len = (size_t)(tab - p);
            if (len >= PKG_NAME_MAX) len = PKG_NAME_MAX - 1;
            strncpy(name, p, len);
            name[len] = '\0';
            p = tab + 1;
        }

        tab = strchr(p, '\t');
        if (tab) {
            size_t len = (size_t)(tab - p);
            if (len >= PKG_VER_MAX) len = PKG_VER_MAX - 1;
            strncpy(ver, p, len);
            ver[len] = '\0';
            p = tab + 1;
        }

        /* Rest is description (strip trailing newline) */
        strncpy(desc, p, PKG_DESC_MAX - 1);
        desc[PKG_DESC_MAX - 1] = '\0';
        size_t dlen = strlen(desc);
        if (dlen > 0 && desc[dlen - 1] == '\n') desc[dlen - 1] = '\0';

        if (name[0]) {
            printf("%-30s %-20s %s\n", name, ver, desc);
            count++;
        }
    }

    fclose(f);
    printf("\n%d package%s installed.\n", count, count == 1 ? "" : "s");
    return 0;
}

/** fusion-pkg search <query> */
static int cmd_search(const char *query) {
    /*
     * In a real implementation this queries the remote repo index.
     * Here we search the installed database as a fallback demonstration.
     */
    printf("%s==>%s Searching for '%s' in installed packages...\n",
           CLR_CYAN, CLR_RESET, query);

    FILE *f = fopen(PKG_DB_LIST, "r");
    if (!f) {
        printf("No packages installed; nothing to search.\n");
        return 0;
    }

    char line[PKG_LINE_MAX];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, query)) {
            char name[PKG_NAME_MAX], ver[PKG_VER_MAX], desc[PKG_DESC_MAX];
            name[0] = ver[0] = desc[0] = '\0';
            sscanf(line, "%127s\t%63s\t%255[^\n]", name, ver, desc);
            printf("  %s%s%s (%s) - %s\n",
                   CLR_GREEN, name, CLR_RESET, ver, desc);
            count++;
        }
    }
    fclose(f);

    if (count == 0) {
        printf("  No matches for '%s'.\n", query);
    } else {
        printf("\n%d match%s found.\n", count, count == 1 ? "" : "es");
    }
    return 0;
}

/** fusion-pkg info <name> */
static int cmd_info(const char *name) {
    FILE *f = fopen(PKG_DB_LIST, "r");
    if (!f) {
        fprintf(stderr, "fusion-pkg: database not found.\n");
        return 1;
    }

    char line[PKG_LINE_MAX];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char pkg_name[PKG_NAME_MAX], ver[PKG_VER_MAX], desc[PKG_DESC_MAX];
        pkg_name[0] = ver[0] = desc[0] = '\0';
        sscanf(line, "%127s\t%63s\t%255[^\n]", pkg_name, ver, desc);
        if (strcmp(pkg_name, name) == 0) {
            printf("%sName:%s        %s\n", CLR_BOLD, CLR_RESET, pkg_name);
            printf("%sVersion:%s     %s\n", CLR_BOLD, CLR_RESET, ver);
            printf("%sDescription:%s %s\n", CLR_BOLD, CLR_RESET, desc);
            printf("%sStatus:%s      %sInstalled%s\n",
                   CLR_BOLD, CLR_RESET, CLR_GREEN, CLR_RESET);
            found = 1;
            break;
        }
    }
    fclose(f);

    if (!found) {
        fprintf(stderr, "fusion-pkg: package '%s' not found.\n", name);
        return 1;
    }
    return 0;
}

/** fusion-pkg update */
static int cmd_update(void) {
    printf("%s==>%s Updating package database...\n", CLR_CYAN, CLR_RESET);

    const char *managers[][3] = {
        { "apt-get", "update", NULL },
        { "pacman",  "-Sy",    NULL },
        { "dnf",     "check-update", NULL },
        { NULL, NULL, NULL },
    };

    for (int i = 0; managers[i][0] != NULL; i++) {
        char *argv[4];
        int j = 0;
        while (managers[i][j]) {
            argv[j] = (char *)managers[i][j];
            j++;
        }
        argv[j] = NULL;

        int rc = run_cmd(argv);
        if (rc == 0 || (i == 2 && rc == 100)) {   /* dnf check-update returns 100 when updates exist */
            printf("%s==>%s Database updated.\n", CLR_GREEN, CLR_RESET);
            return 0;
        }
        if (rc == 127) continue;
    }

    fprintf(stderr, "%sfusion-pkg:%s could not update — "
            "no working package manager found.\n", CLR_RED, CLR_RESET);
    return 1;
}

/* ── Usage / help ──────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("%sFusionOS Package Manager (fusion-pkg)%s\n\n", CLR_BOLD, CLR_RESET);
    printf("Usage: %s <command> [arguments]\n\n", prog);
    printf("Commands:\n");
    printf("  %-22s %s\n", "install <package>",  "Install a package");
    printf("  %-22s %s\n", "remove  <package>",  "Remove an installed package");
    printf("  %-22s %s\n", "list",               "List installed packages");
    printf("  %-22s %s\n", "search  <query>",    "Search for a package");
    printf("  %-22s %s\n", "info    <package>",  "Show package details");
    printf("  %-22s %s\n", "update",             "Update the package database");
    printf("  %-22s %s\n", "help",               "Show this help message");
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "fusion-pkg: install requires a package name.\n");
            return 1;
        }
        return cmd_install(argv[2]);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "fusion-pkg: remove requires a package name.\n");
            return 1;
        }
        return cmd_remove(argv[2]);
    }

    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }

    if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, "fusion-pkg: search requires a query string.\n");
            return 1;
        }
        return cmd_search(argv[2]);
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "fusion-pkg: info requires a package name.\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }

    if (strcmp(cmd, "update") == 0) {
        return cmd_update();
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "fusion-pkg: unknown command '%s'\n", cmd);
    print_usage(argv[0]);
    return 1;
}
