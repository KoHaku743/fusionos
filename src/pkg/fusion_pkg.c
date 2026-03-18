#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * fusion-pkg  —  FusionOS minimal package manager
 *
 * Packages are tarballs fetched with wget/curl and extracted to /.
 * A simple text database at /var/lib/fusion-pkg/installed tracks what
 * has been installed (one package name per line).
 * -------------------------------------------------------------------- */

#define DB_DIR       "/var/lib/fusion-pkg"
#define DB_FILE      "/var/lib/fusion-pkg/installed"
#define CACHE_DIR    "/var/cache/fusion-pkg"
#define PKG_REPO_URL "https://fusionos.example/packages"

/* -----------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------- */

static void ensure_dirs(void)
{
    mkdir(DB_DIR,    0755);
    mkdir(CACHE_DIR, 0755);
}

/* Check whether a package name appears in the installed database. */
static int pkg_is_installed(const char *name)
{
    FILE *f = fopen(DB_FILE, "r");
    if (!f) return 0;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strcmp(line, name) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* Append a package name to the installed database. */
static int pkg_mark_installed(const char *name)
{
    FILE *f = fopen(DB_FILE, "a");
    if (!f) {
        fprintf(stderr, "fusion-pkg: cannot open database %s: %s\n",
                DB_FILE, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\n", name);
    fclose(f);
    return 0;
}

/* Remove a package name from the installed database. */
static int pkg_mark_removed(const char *name)
{
    FILE *fin = fopen(DB_FILE, "r");
    if (!fin) return 0; /* database does not exist — nothing to do */

    char tmp_path[] = DB_DIR "/installed.tmp";
    FILE *fout = fopen(tmp_path, "w");
    if (!fout) {
        fclose(fin);
        fprintf(stderr, "fusion-pkg: cannot write temp db: %s\n", strerror(errno));
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strcmp(line, name) != 0) fprintf(fout, "%s\n", line);
    }
    fclose(fin);
    fclose(fout);
    rename(tmp_path, DB_FILE);
    return 0;
}

/* Run a command via execvp; return its exit status or -1 on error. */
static int run_cmd(const char *const argv[])
{
    pid_t pid = fork();
    if (pid == -1) { perror("fork"); return -1; }

    if (pid == 0) {
        /* POSIX requires char *const[] at exec; the cast is safe here as
         * exec does not modify the strings. */
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

/* -----------------------------------------------------------------------
 * Download helper (tries wget then curl)
 * -------------------------------------------------------------------- */
static int download(const char *url, const char *dest)
{
    const char *wget_args[] = { "wget", "-q", "-O", dest, url, NULL };
    if (run_cmd(wget_args) == 0) return 0;

    const char *curl_args[] = { "curl", "-fsSL", "-o", dest, url, NULL };
    if (run_cmd(curl_args) == 0) return 0;

    fprintf(stderr, "fusion-pkg: download failed for %s\n", url);
    return -1;
}

/* -----------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------- */

static int cmd_install(const char *name)
{
    ensure_dirs();

    if (pkg_is_installed(name)) {
        printf("fusion-pkg: %s is already installed\n", name);
        return 0;
    }

    printf("fusion-pkg: installing %s...\n", name);

    /* Build URL and local cache path */
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s.tar.gz", PKG_REPO_URL, name);

    char cache_path[512];
    snprintf(cache_path, sizeof(cache_path), "%s/%s.tar.gz", CACHE_DIR, name);

    if (download(url, cache_path) != 0) return 1;

    /* Extract to / */
    const char *tar_args[] = { "tar", "-xzf", cache_path, "-C", "/", NULL };
    if (run_cmd(tar_args) != 0) {
        fprintf(stderr, "fusion-pkg: extraction failed for %s\n", name);
        unlink(cache_path);
        return 1;
    }

    unlink(cache_path);

    if (pkg_mark_installed(name) != 0) return 1;

    printf("fusion-pkg: %s installed successfully\n", name);
    return 0;
}

static int cmd_remove(const char *name)
{
    ensure_dirs();

    if (!pkg_is_installed(name)) {
        fprintf(stderr, "fusion-pkg: %s is not installed\n", name);
        return 1;
    }

    printf("fusion-pkg: removing %s...\n", name);
    pkg_mark_removed(name);
    printf("fusion-pkg: %s removed\n", name);
    return 0;
}

static int cmd_list(void)
{
    ensure_dirs();

    FILE *f = fopen(DB_FILE, "r");
    if (!f) {
        printf("fusion-pkg: no packages installed\n");
        return 0;
    }

    printf("Installed packages:\n");
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0]) { printf("  %s\n", line); count++; }
    }
    fclose(f);

    if (count == 0) printf("  (none)\n");
    return 0;
}

static int cmd_update(void)
{
    ensure_dirs();

    FILE *f = fopen(DB_FILE, "r");
    if (!f) {
        printf("fusion-pkg: no packages installed, nothing to update\n");
        return 0;
    }

    char line[256];
    char packages[512][256];
    int count = 0;

    while (fgets(line, sizeof(line), f) && count < 512) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0]) { snprintf(packages[count], sizeof(packages[count]), "%s", line); count++; }
    }
    fclose(f);

    int errors = 0;
    for (int i = 0; i < count; i++) {
        printf("fusion-pkg: updating %s...\n", packages[i]);

        char url[1024];
        snprintf(url, sizeof(url), "%s/%.255s.tar.gz", PKG_REPO_URL, packages[i]);

        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path), "%s/%.255s.tar.gz",
                 CACHE_DIR, packages[i]);

        if (download(url, cache_path) != 0) { errors++; continue; }

        const char *tar_args[] = { "tar", "-xzf", cache_path, "-C", "/", NULL };
        if (run_cmd(tar_args) != 0) {
            fprintf(stderr, "fusion-pkg: update extraction failed for %s\n",
                    packages[i]);
            errors++;
        }
        unlink(cache_path);
    }

    printf("fusion-pkg: update complete (%d error%s)\n",
           errors, errors == 1 ? "" : "s");
    return errors ? 1 : 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [package]\n", prog);
    printf("Commands:\n");
    printf("  install <pkg>  Install a package\n");
    printf("  remove  <pkg>  Remove a package\n");
    printf("  list           List installed packages\n");
    printf("  update         Update all installed packages\n");
    printf("  help           Show this message\n");
}

/* -----------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { fprintf(stderr, "fusion-pkg: install requires a package name\n"); return 1; }
        return cmd_install(argv[2]);
    }

    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { fprintf(stderr, "fusion-pkg: remove requires a package name\n"); return 1; }
        return cmd_remove(argv[2]);
    }

    if (strcmp(cmd, "list") == 0) return cmd_list();
    if (strcmp(cmd, "update") == 0) return cmd_update();
    if (strcmp(cmd, "help") == 0) { print_usage(argv[0]); return 0; }

    fprintf(stderr, "fusion-pkg: unknown command '%s'\n", cmd);
    print_usage(argv[0]);
    return 1;
}
