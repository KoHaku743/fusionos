#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include "fpkg.h"

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

    /*
     * Try each known package manager in order.
     * dnf check-update exits 100 (not 0) when updates are available —
     * treat that as success too.  Use explicit index constants so the
     * dnf check below stays correct even if the array order changes.
     */
#define MGR_APT    0
#define MGR_PACMAN 1
#define MGR_DNF    2

    const char *managers[][3] = {
        [MGR_APT]    = { "apt-get", "update",       NULL },
        [MGR_PACMAN] = { "pacman",  "-Sy",           NULL },
        [MGR_DNF]    = { "dnf",     "check-update",  NULL },
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
        if (rc == 0 ||
            /* dnf check-update exits 100 when updates exist — treat as success */
            (i == MGR_DNF && rc == 100)) {
            printf("%s==>%s Database updated.\n", CLR_GREEN, CLR_RESET);
#undef MGR_APT
#undef MGR_PACMAN
#undef MGR_DNF
            return 0;
        }
        if (rc == 127) continue;
    }

#undef MGR_APT
#undef MGR_PACMAN
#undef MGR_DNF
    fprintf(stderr, "%sfusion-pkg:%s could not update — "
            "no working package manager found.\n", CLR_RED, CLR_RESET);
    return 1;
}

/* ── Compile-time struct size assertions ─────────────────────────────── */

typedef char _assert_header_size [sizeof(FpkgHeader)  == FPKG_HEADER_SIZE ? 1 : -1];
typedef char _assert_entry_size  [sizeof(FpkgFileEntry) == FPKG_ENTRY_SIZE  ? 1 : -1];

/* ── Pure-C SHA-256 ──────────────────────────────────────────────────── */
/*
 * Based on the public-domain description in FIPS PUB 180-4.
 * No external dependencies.
 */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
    uint32_t buf_len;
} Sha256Ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROTR(x,n) (((x) >> (n)) | ((x) << (32-(n))))
#define SHA256_CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_S0(x) (SHA256_ROTR(x,2)^SHA256_ROTR(x,13)^SHA256_ROTR(x,22))
#define SHA256_S1(x) (SHA256_ROTR(x,6)^SHA256_ROTR(x,11)^SHA256_ROTR(x,25))
#define SHA256_s0(x) (SHA256_ROTR(x,7)^SHA256_ROTR(x,18)^((x)>>3))
#define SHA256_s1(x) (SHA256_ROTR(x,17)^SHA256_ROTR(x,19)^((x)>>10))

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4]   << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] <<  8) |
                (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; i++)
        w[i] = SHA256_s1(w[i-2]) + w[i-7] + SHA256_s0(w[i-15]) + w[i-16];

    uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
    uint32_t e=state[4], f=state[5], g=state[6], h=state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SHA256_S1(e) + SHA256_CH(e,f,g) + SHA256_K[i] + w[i];
        uint32_t t2 = SHA256_S0(a) + SHA256_MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->count   = 0;
    ctx->buf_len = 0;
}

static void sha256_update(Sha256Ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->count += (uint64_t)len;
    while (len > 0) {
        uint32_t space = 64 - ctx->buf_len;
        uint32_t copy  = (uint32_t)len < space ? (uint32_t)len : space;
        memcpy(ctx->buf + ctx->buf_len, p, copy);
        ctx->buf_len += copy;
        p   += copy;
        len -= copy;
        if (ctx->buf_len == 64) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t digest[32]) {
    uint64_t bit_count = ctx->count * 8;
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    pad = 0x00;
    while (ctx->buf_len != 56) sha256_update(ctx, &pad, 1);
    /* Big-endian 64-bit bit count */
    for (int i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bit_count & 0xFF);
        sha256_update(ctx, &b, 1);
        bit_count >>= 8;
    }
    for (int i = 0; i < 8; i++) {
        digest[i*4  ] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

/* Compute SHA-256 of the first `size` bytes of open file `fd`. */
static int sha256_file_range(int fd, size_t size, uint8_t digest[32]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    uint8_t   buf[4096];
    size_t    remaining = size;
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    while (remaining > 0) {
        size_t  chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t n     = read(fd, buf, chunk);
        if (n <= 0) return -1;
        sha256_update(&ctx, buf, (size_t)n);
        remaining -= (size_t)n;
    }
    sha256_final(&ctx, digest);
    return 0;
}

/* ── DOS → POSIX path mapping ───────────────────────────────────────── */

/*
 * Convert a DOS path stored in an FpkgFileEntry to a POSIX destination.
 * e.g. "C:\BIN\TOOL.EXE"   → "/usr/local/bin/TOOL.EXE"
 *      "C:\GAMES\DOOM\..."  → "/usr/local/games/DOOM/..."
 *      "C:\DOS\UTIL.COM"    → "/usr/local/dos/UTIL.COM"
 *      "C:\anything\..."    → "/opt/fusionos/anything/..."
 *
 * Returns pointer to out on success, NULL on error.
 */
static char *fpkg_dos_to_posix(const char *dos_path, char *out, size_t out_size) {
    const char *p = dos_path;

    /* Strip optional drive letter "C:" */
    if (p[0] && p[1] == ':') p += 2;
    /* Strip leading slash / backslash */
    while (*p == '/' || *p == '\\') p++;

    /* Find first component */
    const char *sep = p;
    while (*sep && *sep != '\\' && *sep != '/') sep++;

    char first[64];
    size_t flen = (size_t)(sep - p);
    if (flen >= sizeof(first)) flen = sizeof(first) - 1;
    memcpy(first, p, flen);
    first[flen] = '\0';

    /* Remainder after first component */
    const char *rest = *sep ? sep + 1 : sep;

    /* Build the POSIX path, converting backslashes */
    const char *prefix;
    if (strcasecmp(first, "BIN") == 0)
        prefix = FPKG_PREFIX_BIN;
    else if (strcasecmp(first, "DOS") == 0)
        prefix = FPKG_PREFIX_DOS;
    else if (strcasecmp(first, "GAMES") == 0)
        prefix = FPKG_PREFIX_GAMES;
    else {
        /* Construct: /opt/fusionos/<first_component>/<rest> */
        snprintf(out, out_size, "%s/%s/%s", FPKG_PREFIX_ROOT, first, rest);
        /* Replace backslashes */
        for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
        return out;
    }

    if (*rest)
        snprintf(out, out_size, "%s/%s", prefix, rest);
    else
        snprintf(out, out_size, "%s", prefix);

    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
    return out;
}

/* ── File utilities ─────────────────────────────────────────────────── */

/** Recursively create directories for `path`. */
static int mkdirs(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/** Create parent directories for a file path. */
static int mkdirs_for_file(const char *path) {
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return 0;
    memcpy(tmp, path, len + 1);
    char *last = strrchr(tmp, '/');
    if (!last) return 0;
    *last = '\0';
    return mkdirs(tmp);
}

/** Copy exactly `size` bytes from file descriptor `src` to `dst`. */
static int copy_bytes(int src, int dst, uint64_t size) {
    uint8_t buf[65536];
    while (size > 0) {
        size_t  chunk = size < sizeof(buf) ? (size_t)size : sizeof(buf);
        ssize_t n = read(src, buf, chunk);
        if (n <= 0) return -1;
        if (write(dst, buf, (size_t)n) != n) return -1;
        size -= (uint64_t)n;
    }
    return 0;
}

/* ── fpkg_verify ─────────────────────────────────────────────────────── */

/**
 * Open a .fpkg file, validate magic/version, and verify SHA-256.
 *
 * On success, fills `*hdr_out` and leaves the fd positioned right after
 * the header (at the start of the file table).  Caller must close the fd.
 *
 * Returns an open file descriptor on success, or -1 on error.
 */
static int fpkg_open_verify(const char *path, FpkgHeader *hdr_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }

    /* Read and validate header */
    FpkgHeader hdr;
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        fprintf(stderr, "fusion-pkg: '%s': too short to be a valid .fpkg\n", path);
        close(fd); return -1;
    }
    if (memcmp(hdr.magic, FPKG_MAGIC, FPKG_MAGIC_SIZE) != 0) {
        fprintf(stderr, "fusion-pkg: '%s': not a .fpkg file (bad magic)\n", path);
        close(fd); return -1;
    }
    if (hdr.fmt_version != FPKG_FORMAT_VERSION) {
        fprintf(stderr, "fusion-pkg: '%s': unsupported format version %u\n",
                path, hdr.fmt_version);
        close(fd); return -1;
    }

    /* Determine file size */
    off_t total = lseek(fd, 0, SEEK_END);
    if (total < (off_t)(FPKG_HEADER_SIZE + FPKG_SHA256_SIZE)) {
        fprintf(stderr, "fusion-pkg: '%s': file too small\n", path);
        close(fd); return -1;
    }

    /* Verify SHA-256: hash everything except the last 32 bytes */
    size_t covered = (size_t)total - FPKG_SHA256_SIZE;
    uint8_t computed[FPKG_SHA256_SIZE];
    if (sha256_file_range(fd, covered, computed) != 0) {
        fprintf(stderr, "fusion-pkg: '%s': error reading file for checksum\n", path);
        close(fd); return -1;
    }

    /* Read the stored checksum */
    uint8_t stored[FPKG_SHA256_SIZE];
    if (lseek(fd, (off_t)covered, SEEK_SET) < 0 ||
        read(fd, stored, FPKG_SHA256_SIZE) != FPKG_SHA256_SIZE) {
        fprintf(stderr, "fusion-pkg: '%s': cannot read SHA-256 trailer\n", path);
        close(fd); return -1;
    }

    if (memcmp(computed, stored, FPKG_SHA256_SIZE) != 0) {
        fprintf(stderr, "fusion-pkg: '%s': SHA-256 checksum mismatch — "
                "package may be corrupt or tampered\n", path);
        close(fd); return -1;
    }

    /* Seek back to just after header (start of file table) */
    lseek(fd, (off_t)FPKG_HEADER_SIZE, SEEK_SET);

    *hdr_out = hdr;
    return fd;
}

/* ── cmd_verify ──────────────────────────────────────────────────────── */

static int cmd_verify(const char *path) {
    printf("%s==>%s Verifying %s%s%s ...\n",
           CLR_CYAN, CLR_RESET, CLR_BOLD, path, CLR_RESET);

    FpkgHeader hdr;
    int fd = fpkg_open_verify(path, &hdr);
    if (fd < 0) return 1;
    close(fd);

    /* Print basic metadata */
    printf("  Name        : %s\n", hdr.name);
    printf("  Version     : %s\n", hdr.version);
    printf("  Arch        : %s\n", hdr.arch);
    printf("  Description : %s\n", hdr.description);
    printf("  Files       : %u\n", hdr.file_count);
    printf("  Payload     : %llu bytes\n", (unsigned long long)hdr.payload_size);
    printf("\n%sOK%s — package is valid\n", CLR_GREEN, CLR_RESET);
    return 0;
}

/* ── fpkg_install_native ─────────────────────────────────────────────── */

/**
 * Install a .fpkg package natively:
 *   1. Verify checksum
 *   2. Run pre-install script (if any)
 *   3. Extract files to their mapped POSIX destinations
 *   4. Run post-install script (if any)
 *   5. Update fusion-pkg database
 */
static int fpkg_install_native(const char *path) {
    printf("%s==>%s Installing %s%s%s ...\n",
           CLR_CYAN, CLR_RESET, CLR_BOLD, path, CLR_RESET);

    FpkgHeader hdr;
    int fd = fpkg_open_verify(path, &hdr);
    if (fd < 0) return 1;

    /* Already installed? */
    if (db_is_installed(hdr.name)) {
        printf("%s%s%s is already installed.\n",
               CLR_GREEN, hdr.name, CLR_RESET);
        close(fd);
        return 0;
    }

    /* Read file table */
    FpkgFileEntry *entries = NULL;
    if (hdr.file_count > 0) {
        entries = malloc(hdr.file_count * sizeof(FpkgFileEntry));
        if (!entries) { perror("malloc"); close(fd); return 1; }
        if (read(fd, entries, hdr.file_count * FPKG_ENTRY_SIZE) !=
            (ssize_t)(hdr.file_count * FPKG_ENTRY_SIZE)) {
            fprintf(stderr, "fusion-pkg: cannot read file table\n");
            free(entries); close(fd); return 1;
        }
    }

    /* Pre-install script */
    if (hdr.pre_script_size > 0) {
        char script_path[64];
        snprintf(script_path, sizeof(script_path),
                 "/tmp/fpkg-pre-%d.bat", (int)getpid());
        int sfd = open(script_path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (sfd >= 0) {
            if (copy_bytes(fd, sfd, hdr.pre_script_size) != 0)
                fprintf(stderr, "fusion-pkg: warning: pre-install script write failed\n");
            close(sfd);
            printf("  Running pre-install script...\n");
            run_cmd((char *const[]){ "/bin/sh", script_path, NULL });
            unlink(script_path);
        } else {
            /* Skip the pre-install script bytes so we stay in sync */
            lseek(fd, (off_t)hdr.pre_script_size, SEEK_CUR);
        }
    }

    /* Skip post-install script bytes for now (read after payload) */
    off_t post_script_off = (off_t)(hdr.payload_offset +
                                    hdr.payload_size);

    /* Extract files */
    int errors = 0;
    for (uint32_t i = 0; i < hdr.file_count; i++) {
        FpkgFileEntry *e = &entries[i];

        char dest[4096];
        if (!fpkg_dos_to_posix(e->path, dest, sizeof(dest))) {
            fprintf(stderr, "fusion-pkg: bad path in entry %u: %s\n", i, e->path);
            errors++;
            continue;
        }

        if (mkdirs_for_file(dest) != 0) {
            fprintf(stderr, "fusion-pkg: cannot create directory for: %s\n", dest);
            errors++;
            continue;
        }

        int out_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC,
                          (mode_t)(e->mode ? e->mode : 0644));
        if (out_fd < 0) {
            fprintf(stderr, "fusion-pkg: cannot create '%s': %s\n",
                    dest, strerror(errno));
            errors++;
            continue;
        }

        /* Seek to file's data in payload */
        off_t file_off = (off_t)(hdr.payload_offset + e->payload_off);
        if (lseek(fd, file_off, SEEK_SET) < 0 ||
            copy_bytes(fd, out_fd, e->size) != 0) {
            fprintf(stderr, "fusion-pkg: error extracting '%s'\n", dest);
            errors++;
        } else {
            printf("  Installed: %s\n", dest);
        }
        close(out_fd);
    }
    free(entries);

    /* Post-install script */
    if (hdr.post_script_size > 0 && errors == 0) {
        char script_path[64];
        snprintf(script_path, sizeof(script_path),
                 "/tmp/fpkg-post-%d.bat", (int)getpid());
        if (lseek(fd, post_script_off, SEEK_SET) >= 0) {
            int sfd = open(script_path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
            if (sfd >= 0) {
                copy_bytes(fd, sfd, hdr.post_script_size);
                close(sfd);
                printf("  Running post-install script...\n");
                run_cmd((char *const[]){ "/bin/sh", script_path, NULL });
                unlink(script_path);
            }
        }
    }

    close(fd);

    if (errors > 0) {
        fprintf(stderr, "fusion-pkg: %d file(s) failed to install\n", errors);
        return 1;
    }

    /* Record in database */
    Package pkg;
    memset(&pkg, 0, sizeof(pkg));
    snprintf(pkg.name,        sizeof(pkg.name),        "%s", hdr.name);
    snprintf(pkg.version,     sizeof(pkg.version),     "%s", hdr.version);
    snprintf(pkg.description, sizeof(pkg.description), "%s", hdr.description);
    db_record_install(&pkg);

    printf("%s==>%s Successfully installed %s%s%s (%s)\n",
           CLR_GREEN, CLR_RESET, CLR_BOLD, hdr.name, CLR_RESET, hdr.version);
    return 0;
}

/* ── cmd_pack ────────────────────────────────────────────────────────── */

/*
 * Manifest format (key=value, one per line, # comments):
 *
 *   Name=my-tool
 *   Version=1.0.0
 *   Arch=x86_64
 *   Description=A useful FusionOS tool
 *   PreInstall=install.bat     (optional: path to pre-install BAT script)
 *   PostInstall=postinstall.bat (optional)
 *
 * Files directory layout:
 *   files_dir/bin/<name>   → C:\BIN\<NAME>  (executable)
 *   files_dir/dos/<name>   → C:\DOS\<NAME>
 *   files_dir/games/<name> → C:\GAMES\<NAME>
 *   files_dir/<other>      → C:\<OTHER>\<name>
 *
 * Usage:  fusion-pkg pack <manifest> <files_dir> <output.fpkg>
 */

#define PACK_MAX_FILES 4096

typedef struct {
    char   src[4096];         /* Source path on disk           */
    char   dos_path[FPKG_PATH_MAX]; /* Destination DOS path    */
    size_t size;
    mode_t mode;
} PackFile;

/** Recursively collect files from `dir`, building DOS destination paths. */
static int pack_collect(const char *dir, const char *dos_prefix,
                        PackFile *files, int *count) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return -1; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char src[4096];
        snprintf(src, sizeof(src), "%s/%s", dir, de->d_name);

        /* Build uppercase DOS path component */
        char uname[256];
        strncpy(uname, de->d_name, sizeof(uname) - 1);
        uname[sizeof(uname) - 1] = '\0';
        for (char *q = uname; *q; q++) *q = (char)toupper((unsigned char)*q);

        char dos_path[FPKG_PATH_MAX];
        snprintf(dos_path, sizeof(dos_path), "%s\\%s", dos_prefix, uname);

        struct stat st;
        if (stat(src, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse */
            if (pack_collect(src, dos_path, files, count) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (*count >= PACK_MAX_FILES) {
                fprintf(stderr, "fusion-pkg pack: too many files (max %d)\n",
                        PACK_MAX_FILES);
                closedir(d);
                return -1;
            }
            snprintf(files[*count].src,      sizeof(files[*count].src),      "%s", src);
            snprintf(files[*count].dos_path, sizeof(files[*count].dos_path), "%s", dos_path);
            files[*count].size = (size_t)st.st_size;
            files[*count].mode = st.st_mode & 0777;
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int cmd_pack(const char *manifest_path, const char *files_dir,
                    const char *output_path) {
    /* ── Parse manifest ─── */
    FILE *mf = fopen(manifest_path, "r");
    if (!mf) { perror(manifest_path); return 1; }

    char pkg_name[FPKG_NAME_MAX]   = {0};
    char pkg_ver[FPKG_VER_MAX]     = {0};
    char pkg_arch[FPKG_ARCH_MAX]   = "any";
    char pkg_desc[FPKG_DESC_MAX]   = {0};
    char pre_script_file[4096]     = {0};
    char post_script_file[4096]    = {0};

    char line[512];
    while (fgets(line, sizeof(line), mf)) {
        /* Strip newline and trailing whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                           line[len-1] == ' '))
            line[--len] = '\0';
        if (!len || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcasecmp(key, "Name") == 0)
            strncpy(pkg_name, val, sizeof(pkg_name) - 1);
        else if (strcasecmp(key, "Version") == 0)
            strncpy(pkg_ver, val, sizeof(pkg_ver) - 1);
        else if (strcasecmp(key, "Arch") == 0)
            strncpy(pkg_arch, val, sizeof(pkg_arch) - 1);
        else if (strcasecmp(key, "Description") == 0)
            strncpy(pkg_desc, val, sizeof(pkg_desc) - 1);
        else if (strcasecmp(key, "PreInstall") == 0)
            strncpy(pre_script_file, val, sizeof(pre_script_file) - 1);
        else if (strcasecmp(key, "PostInstall") == 0)
            strncpy(post_script_file, val, sizeof(post_script_file) - 1);
    }
    fclose(mf);

    if (!pkg_name[0]) {
        fprintf(stderr, "fusion-pkg pack: manifest missing 'Name'\n");
        return 1;
    }
    if (!pkg_ver[0]) {
        fprintf(stderr, "fusion-pkg pack: manifest missing 'Version'\n");
        return 1;
    }

    printf("%s==>%s Packing %s%s%s v%s\n",
           CLR_CYAN, CLR_RESET, CLR_BOLD, pkg_name, CLR_RESET, pkg_ver);

    /* ── Collect files ─── */
    PackFile *files = calloc(PACK_MAX_FILES, sizeof(PackFile));
    if (!files) { perror("calloc"); return 1; }
    int file_count = 0;

    /* Walk subdirectories: bin/ dos/ games/ and any others */
    DIR *d = opendir(files_dir);
    if (!d) { perror(files_dir); free(files); return 1; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char subdir[4096];
        snprintf(subdir, sizeof(subdir), "%s/%s", files_dir, de->d_name);

        struct stat st;
        if (stat(subdir, &st) != 0) continue;

        char uname[256];
        strncpy(uname, de->d_name, sizeof(uname) - 1);
        uname[sizeof(uname) - 1] = '\0';
        for (char *q = uname; *q; q++) *q = (char)toupper((unsigned char)*q);

        char dos_prefix[FPKG_PATH_MAX];
        snprintf(dos_prefix, sizeof(dos_prefix), "C:\\%s", uname);

        if (S_ISDIR(st.st_mode)) {
            if (pack_collect(subdir, dos_prefix, files, &file_count) != 0) {
                closedir(d); free(files); return 1;
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Top-level file: place under C:\<UNAME> */
            snprintf(files[file_count].src,      sizeof(files[file_count].src),      "%s", subdir);
            snprintf(files[file_count].dos_path, sizeof(files[file_count].dos_path), "%s", dos_prefix);
            files[file_count].size = (size_t)st.st_size;
            files[file_count].mode = st.st_mode & 0777;
            file_count++;
        }
    }
    closedir(d);

    printf("  Collected %d file(s)\n", file_count);

    /* ── Read optional scripts ─── */
    size_t pre_size = 0, post_size = 0;
    char  *pre_data = NULL, *post_data = NULL;

    if (pre_script_file[0]) {
        FILE *sf = fopen(pre_script_file, "r");
        if (!sf) { perror(pre_script_file); free(files); return 1; }
        fseek(sf, 0, SEEK_END); pre_size = (size_t)ftell(sf); fseek(sf, 0, SEEK_SET);
        pre_data = malloc(pre_size);
        if (!pre_data || fread(pre_data, 1, pre_size, sf) != pre_size) {
            perror("fread pre-install"); fclose(sf); free(pre_data); free(files); return 1;
        }
        fclose(sf);
    }

    if (post_script_file[0]) {
        FILE *sf = fopen(post_script_file, "r");
        if (!sf) { perror(post_script_file); free(pre_data); free(files); return 1; }
        fseek(sf, 0, SEEK_END); post_size = (size_t)ftell(sf); fseek(sf, 0, SEEK_SET);
        post_data = malloc(post_size);
        if (!post_data || fread(post_data, 1, post_size, sf) != post_size) {
            perror("fread post-install"); fclose(sf);
            free(post_data); free(pre_data); free(files); return 1;
        }
        fclose(sf);
    }

    /* ── Compute layout ─── */
    uint64_t payload_offset =
        (uint64_t)FPKG_HEADER_SIZE +
        (uint64_t)file_count * FPKG_ENTRY_SIZE +
        (uint64_t)pre_size   +
        (uint64_t)post_size;

    uint64_t payload_size = 0;
    for (int i = 0; i < file_count; i++)
        payload_size += (uint64_t)files[i].size;

    /* ── Build header ─── */
    FpkgHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, FPKG_MAGIC, FPKG_MAGIC_SIZE);
    hdr.fmt_version     = FPKG_FORMAT_VERSION;
    hdr.flags           = FPKG_FLAG_NONE;
    hdr.file_count      = (uint32_t)file_count;
    hdr.pre_script_size = (uint32_t)pre_size;
    hdr.post_script_size= (uint32_t)post_size;
    hdr.payload_offset  = payload_offset;
    hdr.payload_size    = payload_size;
    snprintf(hdr.name,        sizeof(hdr.name),        "%s", pkg_name);
    snprintf(hdr.version,     sizeof(hdr.version),     "%s", pkg_ver);
    snprintf(hdr.arch,        sizeof(hdr.arch),        "%s", pkg_arch);
    snprintf(hdr.description, sizeof(hdr.description), "%s", pkg_desc);

    /* ── Open output file ─── */
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { perror(output_path); free(post_data); free(pre_data); free(files); return 1; }

    /* We compute SHA-256 incrementally as we write. */
    Sha256Ctx sha_ctx;
    sha256_init(&sha_ctx);

    /* Helper: write + feed SHA-256 context, abort on error */
#define WCHK(buf, sz) do { \
    if (write(out_fd, (buf), (sz)) != (ssize_t)(sz)) { \
        perror("write"); close(out_fd); unlink(output_path); \
        free(post_data); free(pre_data); free(files); return 1; \
    } \
    sha256_update(&sha_ctx, (buf), (sz)); \
    } while(0)

    /* Write header */
    WCHK(&hdr, sizeof(hdr));

    /* Write file table */
    uint64_t cur_off = 0;
    for (int i = 0; i < file_count; i++) {
        FpkgFileEntry entry;
        memset(&entry, 0, sizeof(entry));
        snprintf(entry.path, sizeof(entry.path), "%s", files[i].dos_path);
        entry.size        = (uint64_t)files[i].size;
        entry.payload_off = cur_off;
        entry.mode        = (uint32_t)files[i].mode;
        WCHK(&entry, sizeof(entry));
        cur_off += entry.size;
    }

    /* Write pre-install script */
    if (pre_size > 0) WCHK(pre_data, pre_size);

    /* Write post-install script */
    if (post_size > 0) WCHK(post_data, post_size);

    /* Write payload (concatenated file contents) */
    for (int i = 0; i < file_count; i++) {
        int in_fd = open(files[i].src, O_RDONLY);
        if (in_fd < 0) {
            perror(files[i].src);
            close(out_fd); unlink(output_path);
            free(post_data); free(pre_data); free(files);
            return 1;
        }
        /* Stream file contents through SHA-256 and out */
        uint8_t ibuf[65536];
        uint64_t remaining = files[i].size;
        int err = 0;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(ibuf) ? (size_t)remaining : sizeof(ibuf);
            ssize_t n = read(in_fd, ibuf, chunk);
            if (n <= 0) { err = 1; break; }
            if (write(out_fd, ibuf, (size_t)n) != n) { err = 1; break; }
            sha256_update(&sha_ctx, ibuf, (size_t)n);
            remaining -= (uint64_t)n;
        }
        close(in_fd);
        if (err) {
            fprintf(stderr, "fusion-pkg pack: error processing %s\n", files[i].src);
            close(out_fd); unlink(output_path);
            free(post_data); free(pre_data); free(files);
            return 1;
        }
    }
#undef WCHK

    /* ── SHA-256 trailer ─── */
    uint8_t digest[FPKG_SHA256_SIZE];
    sha256_final(&sha_ctx, digest);
    if (write(out_fd, digest, FPKG_SHA256_SIZE) != FPKG_SHA256_SIZE) {
        perror("write sha256 trailer");
        close(out_fd); unlink(output_path);
        free(post_data); free(pre_data); free(files);
        return 1;
    }
    close(out_fd);

    free(post_data);
    free(pre_data);
    free(files);

    /* Print hex digest */
    printf("  SHA-256: ");
    for (int i = 0; i < FPKG_SHA256_SIZE; i++) printf("%02x", digest[i]);
    printf("\n");
    printf("%s==>%s Wrote %s%s%s\n",
           CLR_GREEN, CLR_RESET, CLR_BOLD, output_path, CLR_RESET);
    return 0;
}

/* ── Usage / help ──────────────────────────────────────────────────── */

static void print_usage(const char *prog) {
    printf("%sFusionOS Package Manager (fusion-pkg)%s\n\n", CLR_BOLD, CLR_RESET);
    printf("Usage: %s <command> [arguments]\n\n", prog);
    printf("Commands:\n");
    printf("  %-30s %s\n", "install <package|file.fpkg>",
           "Install from repo or native .fpkg file");
    printf("  %-30s %s\n", "remove  <package>",
           "Remove an installed package");
    printf("  %-30s %s\n", "list",
           "List installed packages");
    printf("  %-30s %s\n", "search  <query>",
           "Search for a package");
    printf("  %-30s %s\n", "info    <package>",
           "Show package details");
    printf("  %-30s %s\n", "update",
           "Update the package database");
    printf("  %-30s %s\n", "verify  <file.fpkg>",
           "Verify .fpkg integrity (magic + SHA-256)");
    printf("  %-30s %s\n", "pack <manifest> <dir> <out.fpkg>",
           "Create a .fpkg from a staging directory");
    printf("  %-30s %s\n", "help",
           "Show this help message");
    printf("\nNative .fpkg format v1:\n");
    printf("  Header(520B) + FileTable + PreScript + PostScript + Payload + SHA-256(32B)\n");
    printf("  Install destinations:\n");
    printf("    C:\\BIN\\*   -> %s/\n", FPKG_PREFIX_BIN);
    printf("    C:\\DOS\\*   -> %s/\n", FPKG_PREFIX_DOS);
    printf("    C:\\GAMES\\* -> %s/\n", FPKG_PREFIX_GAMES);
    printf("    C:\\*       -> %s/<subdir>/\n", FPKG_PREFIX_ROOT);
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
            fprintf(stderr, "fusion-pkg: install requires a package name or .fpkg file.\n");
            return 1;
        }
        /* Detect .fpkg extension (case-insensitive) */
        const char *arg = argv[2];
        size_t alen = strlen(arg);
        if (alen > 5) {
            const char *ext = arg + alen - 5;
            if (strcasecmp(ext, ".fpkg") == 0)
                return fpkg_install_native(arg);
        }
        return cmd_install(arg);
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

    if (strcmp(cmd, "verify") == 0) {
        if (argc < 3) {
            fprintf(stderr, "fusion-pkg: verify requires a .fpkg file path.\n");
            return 1;
        }
        return cmd_verify(argv[2]);
    }

    if (strcmp(cmd, "pack") == 0) {
        if (argc < 5) {
            fprintf(stderr, "fusion-pkg pack: usage: pack <manifest> <files_dir> <output.fpkg>\n");
            return 1;
        }
        return cmd_pack(argv[2], argv[3], argv[4]);
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
