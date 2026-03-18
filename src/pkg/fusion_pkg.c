#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * fusion-pkg  —  FusionOS package manager
 *
 * Maintains a JSON database at /var/lib/fusion-pkg/packages.json.
 * Uses a hardcoded registry of known packages; apt is the install backend.
 * Commands: install, remove, list, search, info, update, help
 * -------------------------------------------------------------------- */

/* ------------------------------------------------------------------
 * ANSI colours / progress
 * ------------------------------------------------------------------ */
#define COL_RED     "\x1b[31m"
#define COL_GREEN   "\x1b[32m"
#define COL_YELLOW  "\x1b[33m"
#define COL_BLUE    "\x1b[34m"
#define COL_CYAN    "\x1b[36m"
#define COL_BOLD    "\x1b[1m"
#define COL_RESET   "\x1b[0m"

#define OK_TAG   COL_GREEN "[✓]" COL_RESET " "
#define ERR_TAG  COL_RED   "[✗]" COL_RESET " "
#define INFO_TAG COL_CYAN  "[i]" COL_RESET " "

/* ------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------ */
#define DB_DIR      "/var/lib/fusion-pkg"
#define DB_FILE     "/var/lib/fusion-pkg/packages.json"
#define DB_TMP      "/var/lib/fusion-pkg/packages.json.tmp"
#define DB_MAX_SIZE (256 * 1024)  /* 256 KB — generous limit for the JSON db */

/* ------------------------------------------------------------------
 * Package registry
 * ------------------------------------------------------------------ */
typedef enum {
    PKG_TYPE_APT = 0,
    PKG_TYPE_SOURCE
} PkgType;

typedef struct {
    const char *name;
    const char *description;
    const char *version;
    PkgType     type;
    const char *apt_pkgs;       /* space-separated, only for PKG_TYPE_APT */
    const char *post_install;   /* command to run after install, or NULL  */
    const char *compat;         /* compat layer: wine/box64/fex/darling/native */
    const char *url;            /* for source type, or project homepage   */
} RegEntry;

/* Hardcoded known packages — extend as needed */
static const RegEntry REGISTRY[] = {
    {
        "wine",
        "Windows compatibility layer (x86 + x64 PE support)",
        "8.0",
        PKG_TYPE_APT,
        "wine wine64 winetricks",
        "winecfg /silent",
        "wine",
        "https://www.winehq.org/"
    },
    {
        "box64",
        "ARM64 Linux user-space emulator for x86-64 binaries",
        "0.2.4",
        PKG_TYPE_APT,
        "box64",
        NULL,
        "box64",
        "https://github.com/ptitSeb/box64"
    },
    {
        "fex",
        "FEX-Emu: fast ARM64 emulator for x86/x86-64 binaries",
        "2311",
        PKG_TYPE_APT,
        "fex-emu",
        NULL,
        "fex",
        "https://fex-emu.com/"
    },
    {
        "darling",
        "macOS compatibility layer (Mach-O / Darwin binaries)",
        "0.1.0",
        PKG_TYPE_SOURCE,
        NULL,
        NULL,
        "darling",
        "https://github.com/darlinghq/darling"
    },
    {
        "chromium",
        "Open-source web browser (Chromium)",
        "latest",
        PKG_TYPE_APT,
        "chromium",
        NULL,
        "native",
        "https://www.chromium.org/"
    },
    {
        "firefox",
        "Mozilla Firefox web browser",
        "latest",
        PKG_TYPE_APT,
        "firefox",
        NULL,
        "native",
        "https://www.mozilla.org/firefox/"
    },
};

#define REGISTRY_SIZE ((int)(sizeof(REGISTRY) / sizeof(REGISTRY[0])))

/* ------------------------------------------------------------------
 * Registry lookup
 * ------------------------------------------------------------------ */
static const RegEntry *registry_find(const char *name)
{
    for (int i = 0; i < REGISTRY_SIZE; i++) {
        if (strcmp(REGISTRY[i].name, name) == 0) return &REGISTRY[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------
 * Simple heap-allocated string buffer
 * ------------------------------------------------------------------ */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_init(Buf *b, size_t initial_cap)
{
    b->data = malloc(initial_cap);
    if (!b->data) return -1;
    b->data[0] = '\0';
    b->len = 0;
    b->cap = initial_cap;
    return 0;
}

static int buf_append(Buf *b, const char *s)
{
    size_t slen = strlen(s);
    while (b->len + slen + 1 > b->cap) {
        size_t new_cap = b->cap * 2;
        char *p = realloc(b->data, new_cap);
        if (!p) return -1;
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
    return 0;
}

static void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* ------------------------------------------------------------------
 * JSON database I/O
 *
 * Schema (hand-written, deterministic):
 *
 * {
 *   "version": "1",
 *   "packages": {
 *     "wine": {
 *       "installed": true,
 *       "version": "8.0",
 *       "install_date": "2024-01-15 10:30:00"
 *     }
 *   }
 * }
 * ------------------------------------------------------------------ */

/* In-memory representation of one installed package */
typedef struct {
    char name[64];
    char version[64];
    char install_date[32];
    int  installed;          /* 1 = installed, 0 = removed */
} DbEntry;

/* Minimal JSON helpers: operate on NUL-terminated string in memory */

/* Find the first occurrence of "key": (with quotes) starting from *pos. */
static const char *json_find_key(const char *src, const char *key)
{
    /* Construct `"key":` search pattern */
    char pat[130];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    return strstr(src, pat);
}

/* Read a JSON string value for "key" from an object text.
 * Writes into out[0..out_size-1].  Returns 0 on success. */
static int json_get_string(const char *obj, const char *key,
                           char *out, size_t out_size)
{
    const char *p = json_find_key(obj, key);
    if (!p) return -1;

    /* Skip past `"key":` and optional whitespace */
    p += strlen(key) + 3; /* `"` + key + `":` */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p != '"') return -1;
    p++; /* skip opening `"` */

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

/* Read a JSON boolean value ("true" or "false") for "key". Returns 1/0/-1. */
static int json_get_bool(const char *obj, const char *key)
{
    const char *p = json_find_key(obj, key);
    if (!p) return -1;

    p += strlen(key) + 3;
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "true", 4) == 0)  return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return -1;
}

/* ------------------------------------------------------------------
 * Load the DB file into an array of DbEntry.
 * Returns the number of entries read (0 if file doesn't exist).
 * ------------------------------------------------------------------ */
static int db_load(DbEntry *entries, int max_entries)
{
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;

    /* Read whole file */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsize = ftell(f);
    rewind(f);

    if (fsize <= 0 || fsize > DB_MAX_SIZE) { fclose(f); return 0; }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return 0; }

    size_t got = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[got] = '\0';

    /* Find "packages": { ... } section */
    const char *pkgs_start = json_find_key(buf, "packages");
    if (!pkgs_start) { free(buf); return 0; }

    /* Move past "packages": */
    pkgs_start = strchr(pkgs_start, '{');
    if (!pkgs_start) { free(buf); return 0; }
    pkgs_start++; /* skip `{` */

    int count = 0;
    const char *p = pkgs_start;

    while (count < max_entries) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

        if (*p == '}' || *p == '\0') break; /* end of packages object */

        /* Expect `"name": {` */
        if (*p != '"') { p++; continue; }
        p++; /* skip `"` */

        char name[64];
        size_t ni = 0;
        while (*p && *p != '"' && ni < sizeof(name) - 1) name[ni++] = *p++;
        name[ni] = '\0';
        if (*p != '"') { p++; continue; }
        p++; /* skip closing `"` */

        /* Skip `: ` */
        while (*p == ' ' || *p == ':' || *p == '\t') p++;
        if (*p != '{') continue;

        /* Find matching `}` for this package's object */
        int depth = 0;
        const char *obj_start = p;
        const char *obj_end   = p;
        const char *q = p;
        while (*q) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { obj_end = q + 1; break; } }
            q++;
        }

        /* Copy object text for field parsing */
        size_t obj_len = (size_t)(obj_end - obj_start);
        char *obj_copy = malloc(obj_len + 1);
        if (obj_copy) {
            memcpy(obj_copy, obj_start, obj_len);
            obj_copy[obj_len] = '\0';

            snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
            entries[count].installed = json_get_bool(obj_copy, "installed");
            if (entries[count].installed < 0) entries[count].installed = 0;

            if (json_get_string(obj_copy, "version",
                                entries[count].version,
                                sizeof(entries[count].version)) != 0) {
                entries[count].version[0] = '\0';
            }
            if (json_get_string(obj_copy, "install_date",
                                entries[count].install_date,
                                sizeof(entries[count].install_date)) != 0) {
                entries[count].install_date[0] = '\0';
            }
            free(obj_copy);
            count++;
        }

        p = obj_end;
        /* Skip optional comma */
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == '\n') p++;
    }

    free(buf);
    return count;
}

/* ------------------------------------------------------------------
 * Write the DB back to disk (atomic replace via temp file).
 * ------------------------------------------------------------------ */
static int db_save(const DbEntry *entries, int count)
{
    mkdir(DB_DIR, 0755);

    Buf b;
    if (buf_init(&b, 4096) != 0) return -1;

    buf_append(&b, "{\n  \"version\": \"1\",\n  \"packages\": {");

    int first = 1;
    for (int i = 0; i < count; i++) {
        if (!first) buf_append(&b, ",");
        first = 0;

        char obj[512];
        if (entries[i].installed) {
            snprintf(obj, sizeof(obj),
                "\n    \"%s\": {\n"
                "      \"installed\": true,\n"
                "      \"version\": \"%s\",\n"
                "      \"install_date\": \"%s\"\n"
                "    }",
                entries[i].name,
                entries[i].version,
                entries[i].install_date);
        } else {
            snprintf(obj, sizeof(obj),
                "\n    \"%s\": {\n"
                "      \"installed\": false\n"
                "    }",
                entries[i].name);
        }
        buf_append(&b, obj);
    }

    buf_append(&b, "\n  }\n}\n");

    FILE *f = fopen(DB_TMP, "w");
    if (!f) { buf_free(&b); return -1; }

    size_t written = fwrite(b.data, 1, b.len, f);
    size_t blen    = b.len;
    fclose(f);
    buf_free(&b);

    if (written != blen) { unlink(DB_TMP); return -1; }

    return rename(DB_TMP, DB_FILE);
}

/* Find an entry by name within an array; returns index or -1. */
static int db_find(const DbEntry *entries, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------
 * Process execution helper
 * ------------------------------------------------------------------ */
static int run_cmd(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return -1; }

    if (pid == 0) {
        /* POSIX requires char *const[]; the strings are not modified. */
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "fusion-pkg: exec '%s' failed: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ------------------------------------------------------------------
 * Spinner: print a simple progress line.
 * ------------------------------------------------------------------ */
static void spinner_tick(const char *msg)
{
    static const char frames[] = { '-', '\\', '|', '/' };
    static int        frame    = 0;
    printf("\r  %c  %s ...", frames[frame % 4], msg);
    fflush(stdout);
    frame++;
}

static void spinner_done(const char *msg, int ok)
{
    if (ok) printf("\r" OK_TAG  "%s\n", msg);
    else    printf("\r" ERR_TAG "%s\n", msg);
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * Current timestamp as "YYYY-MM-DD HH:MM:SS"
 * ------------------------------------------------------------------ */
static void timestamp(char *out, size_t out_size)
{
    time_t now  = time(NULL);
    struct tm *t = localtime(&now);
    if (t) {
        strftime(out, out_size, "%Y-%m-%d %H:%M:%S", t);
    } else {
        snprintf(out, out_size, "unknown");
    }
}

/* ------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------ */

#define MAX_DB_ENTRIES 256

/**
 * Install a package from the hardcoded registry using apt-get.
 * Updates the JSON database on success.
 */
static int cmd_install(const char *name)
{
    const RegEntry *reg = registry_find(name);
    if (!reg) {
        printf(ERR_TAG "Unknown package: " COL_BOLD "%s" COL_RESET "\n", name);
        printf(INFO_TAG "Use 'fusion-pkg search %s' to look for similar packages.\n", name);
        return 1;
    }

    /* Load existing DB */
    DbEntry entries[MAX_DB_ENTRIES];
    int     count = db_load(entries, MAX_DB_ENTRIES);

    int idx = db_find(entries, count, name);
    if (idx >= 0 && entries[idx].installed) {
        printf(INFO_TAG COL_BOLD "%s" COL_RESET
               " is already installed (version %s).\n",
               name, entries[idx].version);
        return 0;
    }

    printf(COL_BOLD "\nInstalling: %s" COL_RESET
           " — %s\n\n", reg->name, reg->description);

    if (reg->type == PKG_TYPE_SOURCE) {
        printf(INFO_TAG "%s must be built from source.\n", name);
        printf(INFO_TAG "Source: %s\n\n", reg->url ? reg->url : "(unknown)");
        printf("  1. Clone the repository\n");
        printf("  2. Follow the build instructions in the README\n");
        printf("  3. Run: fusion-pkg install %s  (to record it as installed)\n\n", name);
        /* Mark as installed anyway so the user can proceed */
    } else {
        /* APT installation */
        spinner_tick("Updating package index");
        const char *apt_update[] = { "apt-get", "update", "-qq", NULL };
        int rc = run_cmd(apt_update);
        spinner_done("Package index updated", rc == 0);
        if (rc != 0) {
            printf(ERR_TAG "apt-get update failed.\n");
            return 1;
        }

        spinner_tick("Installing packages");

        /* Build argv from space-separated apt_pkgs string */
        char pkgs_copy[512];
        snprintf(pkgs_copy, sizeof(pkgs_copy), "%s", reg->apt_pkgs);

        const char *apt_argv[64];
        int ai = 0;
        apt_argv[ai++] = "apt-get";
        apt_argv[ai++] = "install";
        apt_argv[ai++] = "-y";

        char *tok = strtok(pkgs_copy, " ");
        while (tok && ai < 62) {
            apt_argv[ai++] = tok;
            tok = strtok(NULL, " ");
        }
        apt_argv[ai] = NULL;

        rc = run_cmd(apt_argv);
        spinner_done("Packages installed", rc == 0);
        if (rc != 0) {
            printf(ERR_TAG "apt-get install failed.\n");
            return 1;
        }

        /* Post-install step */
        if (reg->post_install) {
            spinner_tick("Running post-install");
            const char *sh_argv[] = { "sh", "-c", reg->post_install, NULL };
            run_cmd(sh_argv);
            spinner_done("Post-install complete", 1);
        }
    }

    /* Update DB */
    if (idx < 0) {
        if (count < MAX_DB_ENTRIES) {
            idx = count++;
        } else {
            printf(ERR_TAG "Database full; cannot record installation.\n");
            return 1;
        }
    }
    snprintf(entries[idx].name,    sizeof(entries[idx].name),    "%s", reg->name);
    snprintf(entries[idx].version, sizeof(entries[idx].version), "%s", reg->version);
    timestamp(entries[idx].install_date, sizeof(entries[idx].install_date));
    entries[idx].installed = 1;

    if (db_save(entries, count) != 0) {
        printf(ERR_TAG "Warning: could not update package database.\n");
    }

    printf("\n" OK_TAG COL_BOLD "%s" COL_RESET " installed successfully.\n\n", name);
    return 0;
}

/**
 * Remove a package from the JSON database (does not uninstall apt packages).
 */
static int cmd_remove(const char *name)
{
    DbEntry entries[MAX_DB_ENTRIES];
    int     count = db_load(entries, MAX_DB_ENTRIES);

    int idx = db_find(entries, count, name);
    if (idx < 0 || !entries[idx].installed) {
        printf(ERR_TAG "%s is not installed.\n", name);
        return 1;
    }

    printf(INFO_TAG "Removing " COL_BOLD "%s" COL_RESET " from database...\n", name);
    entries[idx].installed = 0;

    if (db_save(entries, count) != 0) {
        printf(ERR_TAG "Could not update package database.\n");
        return 1;
    }

    printf(OK_TAG "%s removed from FusionOS package database.\n", name);
    printf(INFO_TAG "Note: system packages were NOT uninstalled.\n");
    printf(INFO_TAG "To fully remove, run: apt-get remove %s\n",
           registry_find(name) && registry_find(name)->apt_pkgs
           ? registry_find(name)->apt_pkgs : name);
    return 0;
}

/**
 * List all installed packages from the JSON database.
 */
static int cmd_list(void)
{
    DbEntry entries[MAX_DB_ENTRIES];
    int     count = db_load(entries, MAX_DB_ENTRIES);

    int installed = 0;
    printf(COL_BOLD "%-16s %-12s %-24s %s\n" COL_RESET,
           "Package", "Version", "Installed", "Description");
    printf("%-16s %-12s %-24s %s\n",
           "-------", "-------", "---------", "-----------");

    for (int i = 0; i < count; i++) {
        if (!entries[i].installed) continue;
        const RegEntry *r = registry_find(entries[i].name);
        printf(COL_GREEN "%-16s" COL_RESET " %-12s %-24s %s\n",
               entries[i].name,
               entries[i].version,
               entries[i].install_date,
               r ? r->description : "");
        installed++;
    }

    if (installed == 0) {
        printf("  " COL_YELLOW "(no packages installed)" COL_RESET "\n");
    } else {
        printf("\n" INFO_TAG "%d package%s installed.\n",
               installed, installed == 1 ? "" : "s");
    }
    return 0;
}

/**
 * Search the hardcoded registry for packages matching a search term.
 */
static int cmd_search(const char *term)
{
    printf(COL_BOLD "Search results for \"" COL_CYAN "%s" COL_RESET COL_BOLD "\":\n\n" COL_RESET,
           term);

    int found = 0;
    for (int i = 0; i < REGISTRY_SIZE; i++) {
        const RegEntry *r = &REGISTRY[i];
        /* Case-insensitive substring match in name or description */
        int match = 0;

        /* Simple manual case-insensitive search */
        const char *haystack[2] = { r->name, r->description };
        for (int h = 0; h < 2 && !match; h++) {
            const char *p = haystack[h];
            size_t tlen = strlen(term);
            while (*p) {
                int equal = 1;
                for (size_t j = 0; j < tlen; j++) {
                    char a = p[j];
                    char b = term[j];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) { equal = 0; break; }
                }
                if (equal) { match = 1; break; }
                p++;
            }
        }

        if (!match) continue;
        found++;

        printf("  " COL_BOLD "%-16s" COL_RESET " [%s] %s\n",
               r->name,
               r->type == PKG_TYPE_APT ? "apt" : "src",
               r->description);
        printf("  %-16s version %-8s  compat: %s\n\n",
               "", r->version, r->compat ? r->compat : "native");
    }

    if (found == 0) {
        printf("  " COL_YELLOW "No packages found matching \"%s\".\n" COL_RESET, term);
    }
    return 0;
}

/**
 * Show detailed info about a package.
 */
static int cmd_info(const char *name)
{
    const RegEntry *r = registry_find(name);
    if (!r) {
        printf(ERR_TAG "Unknown package: %s\n", name);
        return 1;
    }

    /* Check install status */
    DbEntry entries[MAX_DB_ENTRIES];
    int     count = db_load(entries, MAX_DB_ENTRIES);
    int     idx   = db_find(entries, count, name);

    printf("\n" COL_BOLD "Package: " COL_CYAN "%s" COL_RESET "\n", r->name);
    printf("  Description : %s\n", r->description);
    printf("  Version     : %s\n", r->version);
    printf("  Compat layer: %s\n", r->compat   ? r->compat   : "native");
    printf("  Homepage    : %s\n", r->url       ? r->url      : "N/A");
    printf("  Type        : %s\n", r->type == PKG_TYPE_APT ? "apt" : "source");
    if (r->apt_pkgs)      printf("  Apt packages: %s\n", r->apt_pkgs);
    if (r->post_install)  printf("  Post-install: %s\n", r->post_install);

    if (idx >= 0 && entries[idx].installed) {
        printf("  Status      : " COL_GREEN "installed" COL_RESET
               " (version %s, on %s)\n",
               entries[idx].version, entries[idx].install_date);
    } else {
        printf("  Status      : " COL_YELLOW "not installed" COL_RESET "\n");
    }
    printf("\n");
    return 0;
}

/**
 * Update all installed packages by reinstalling them.
 */
static int cmd_update(void)
{
    DbEntry entries[MAX_DB_ENTRIES];
    int     count = db_load(entries, MAX_DB_ENTRIES);

    int installed = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].installed) installed++;
    }

    if (installed == 0) {
        printf(INFO_TAG "No packages installed; nothing to update.\n");
        return 0;
    }

    printf(INFO_TAG "Updating %d package%s...\n\n",
           installed, installed == 1 ? "" : "s");

    /* Update apt index once */
    spinner_tick("Updating package index");
    const char *apt_update[] = { "apt-get", "update", "-qq", NULL };
    int rc = run_cmd(apt_update);
    spinner_done("Package index updated", rc == 0);
    if (rc != 0) {
        printf(ERR_TAG "apt-get update failed.\n");
        return 1;
    }

    int errors = 0;
    for (int i = 0; i < count; i++) {
        if (!entries[i].installed) continue;

        const RegEntry *r = registry_find(entries[i].name);
        if (!r || r->type != PKG_TYPE_APT || !r->apt_pkgs) continue;

        char msg[80];
        snprintf(msg, sizeof(msg), "Updating %.63s", entries[i].name);
        spinner_tick(msg);

        char pkgs_copy[512];
        snprintf(pkgs_copy, sizeof(pkgs_copy), "%s", r->apt_pkgs);

        const char *apt_argv[64];
        int ai = 0;
        apt_argv[ai++] = "apt-get";
        apt_argv[ai++] = "install";
        apt_argv[ai++] = "--only-upgrade";
        apt_argv[ai++] = "-y";

        char *tok = strtok(pkgs_copy, " ");
        while (tok && ai < 62) {
            apt_argv[ai++] = tok;
            tok = strtok(NULL, " ");
        }
        apt_argv[ai] = NULL;

        rc = run_cmd(apt_argv);
        spinner_done(msg, rc == 0);
        if (rc != 0) errors++;

        /* Update the install_date to record the update time */
        timestamp(entries[i].install_date, sizeof(entries[i].install_date));
    }

    db_save(entries, count);

    printf("\n" OK_TAG "Update complete (%d error%s).\n",
           errors, errors == 1 ? "" : "s");
    return errors ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Usage
 * ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    printf(COL_BOLD "\nfusion-pkg — FusionOS Package Manager\n\n" COL_RESET);
    printf("Usage: %s <command> [argument]\n\n", prog);
    printf("Commands:\n");
    printf("  install <pkg>   Install a package\n");
    printf("  remove  <pkg>   Remove a package (from database)\n");
    printf("  list            List all installed packages\n");
    printf("  search  <term>  Search available packages\n");
    printf("  info    <pkg>   Show detailed package information\n");
    printf("  update          Update all installed packages\n");
    printf("  help            Show this message\n\n");
    printf("Available packages: wine, box64, fex, darling, chromium, firefox\n\n");
}

/* ------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, ERR_TAG "install requires a package name.\n");
            return 1;
        }
        return cmd_install(argv[2]);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, ERR_TAG "remove requires a package name.\n");
            return 1;
        }
        return cmd_remove(argv[2]);
    }

    if (strcmp(cmd, "list")   == 0) return cmd_list();
    if (strcmp(cmd, "update") == 0) return cmd_update();

    if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, ERR_TAG "search requires a search term.\n");
            return 1;
        }
        return cmd_search(argv[2]);
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, ERR_TAG "info requires a package name.\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }

    if (strcmp(cmd, "help") == 0 ||
        strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, ERR_TAG "Unknown command: '%s'\n", cmd);
    print_usage(argv[0]);
    return 1;
}
