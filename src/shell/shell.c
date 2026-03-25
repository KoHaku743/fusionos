#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <signal.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "../compat/detect.h"

/*
 * FusionOS COMMAND.COM  --  An evolved DOS shell for 2025
 *
 * Design goals:
 *   - Feels like MS-DOS COMMAND.COM to the user (C:\> prompt, DOS commands)
 *   - Adds modern usability: history, aliases, %VAR%, batch scripting
 *   - Transparently routes foreign binaries (PE/Mach-O) through compat layer
 *   - Scripts are plain .BAT files (DOS batch syntax)
 */

/* ── Constants ──────────────────────────────────────────────────────── */
#define VERSION_STR   "FusionOS COMMAND.COM Version 1.00"
#define MAX_CMD_LEN   4096
#define MAX_ARGS      128
#define HISTORY_SIZE  256
#define MAX_ENV       256
#define MAX_PATH_LEN  4096
#define MAX_BATCH_DEPTH 8  /* max .BAT nesting */

/* ── Types ──────────────────────────────────────────────────────────── */
typedef struct {
    char key[64];
    char val[256];
} EnvVar;

typedef struct {
    char name[64];
    char expansion[MAX_CMD_LEN];
} Alias;

#define MAX_ALIASES 64

/* ── Job control ─────────────────────────────────────────────────────── */

/**
 * Priority levels for launched processes.
 * REALTIME maps to SCHED_FIFO (requires root / CAP_SYS_NICE).
 * HIGH and NORMAL use setpriority nice-value adjustments.
 */
typedef enum {
    PRIO_NORMAL   = 0,
    PRIO_HIGH     = 1,
    PRIO_REALTIME = 2,
} ExecPriority;

#define MAX_JOBS 32

typedef struct {
    int          active;
    pid_t        pid;
    char         name[64];    /* command name */
    ExecPriority priority;
    int          stopped;     /* 1 if sent SIGSTOP */
} Job;

static Job jobs[MAX_JOBS];

/** Add a background job to the table.  Returns job index (1-based) or -1. */
static int jobs_add(pid_t pid, const char *name, ExecPriority prio) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) {
            jobs[i].active   = 1;
            jobs[i].pid      = pid;
            jobs[i].priority = prio;
            jobs[i].stopped  = 0;
            size_t nlen = strlen(name);
            if (nlen >= sizeof(jobs[i].name)) nlen = sizeof(jobs[i].name) - 1;
            memcpy(jobs[i].name, name, nlen);
            jobs[i].name[nlen] = '\0';
            return i + 1;  /* 1-based job number */
        }
    }
    return -1;  /* table full */
}

/** Reap any finished background jobs (non-blocking). */
static void reap_jobs(void) {
    int    status;
    pid_t  pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].active && jobs[i].pid == pid) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                printf("\n[%d]  Done (%d): %s\n",
                       i + 1, code, jobs[i].name);
                jobs[i].active = 0;
                break;
            }
        }
    }
}

/** JOBS command — list active background jobs. */
static void cmd_jobs(void) {
    int any = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].active) continue;
        const char *prio_str =
            jobs[i].priority == PRIO_REALTIME ? "REALTIME" :
            jobs[i].priority == PRIO_HIGH     ? "HIGH"     : "NORMAL";
        const char *state = jobs[i].stopped ? "Stopped" : "Running";
        printf("[%d]  %s  PID %-8d  %-8s  %s\n",
               i + 1, state, jobs[i].pid, prio_str, jobs[i].name);
        any = 1;
    }
    if (!any) printf("No background jobs.\n");
}

/** Parse a job specifier: %N → index N (1-based), numeric → PID. */
static pid_t resolve_job_spec(const char *spec) {
    if (spec[0] == '%') {
        int idx = atoi(spec + 1) - 1;
        if (idx >= 0 && idx < MAX_JOBS && jobs[idx].active)
            return jobs[idx].pid;
        return -1;
    }
    return (pid_t)atoi(spec);
}

/** KILL command. */
static void cmd_kill_job(const char *spec, int sig) {
    if (!spec || !*spec) {
        fprintf(stderr, "KILL: missing PID or %%job\n");
        return;
    }
    pid_t pid = resolve_job_spec(spec);
    if (pid <= 0) {
        fprintf(stderr, "KILL: invalid job spec: %s\n", spec);
        return;
    }
    if (kill(pid, sig) != 0)
        perror("kill");
    else {
        if (sig == SIGTERM || sig == SIGKILL) {
            /* Mark job inactive */
            for (int i = 0; i < MAX_JOBS; i++) {
                if (jobs[i].active && jobs[i].pid == pid) {
                    jobs[i].active = 0;
                    break;
                }
            }
        }
    }
}

/** PAUSE command (suspend a background job). */
static void cmd_pause_job(const char *spec) {
    /* No argument: interactive "press any key" pause */
    if (!spec || !*spec) {
        printf("Press any key to continue . . . ");
        fflush(stdout);
        getchar();
        putchar('\n');
        return;
    }
    pid_t pid = resolve_job_spec(spec);
    if (pid <= 0) {
        fprintf(stderr, "PAUSE: invalid job spec: %s\n", spec);
        return;
    }
    if (kill(pid, SIGSTOP) != 0) { perror("kill"); return; }
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) { jobs[i].stopped = 1; break; }
    }
    printf("[Stopped] PID %d\n", pid);
}

/** RESUME command (continue a stopped background job). */
static void cmd_resume_job(const char *spec) {
    if (!spec || !*spec) { fprintf(stderr, "RESUME: missing job spec\n"); return; }
    pid_t pid = resolve_job_spec(spec);
    if (pid <= 0) { fprintf(stderr, "RESUME: invalid job spec: %s\n", spec); return; }
    if (kill(pid, SIGCONT) != 0) { perror("kill"); return; }
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].active && jobs[i].pid == pid) { jobs[i].stopped = 0; break; }
    }
    printf("[Continued] PID %d\n", pid);
}

/* ── Globals ────────────────────────────────────────────────────────── */
static char  history[HISTORY_SIZE][MAX_CMD_LEN];
static int   history_count = 0;
static int   echo_on       = 1; /* ECHO ON/OFF state */
static int   last_errorlevel = 0;

static EnvVar env_vars[MAX_ENV];
static int    env_count = 0;

static Alias aliases[MAX_ALIASES];
static int   alias_count = 0;

/* ── Environment helpers ─────────────────────────────────────────────── */

static const char *env_get(const char *key) {
    /* Check our table first */
    for (int i = 0; i < env_count; i++) {
        if (strcasecmp(env_vars[i].key, key) == 0)
            return env_vars[i].val;
    }
    /* Fall back to process environment */
    return getenv(key);
}

static void env_set(const char *key, const char *val) {
    for (int i = 0; i < env_count; i++) {
        if (strcasecmp(env_vars[i].key, key) == 0) {
            snprintf(env_vars[i].val, sizeof(env_vars[i].val), "%s", val);
            setenv(key, val, 1);
            return;
        }
    }
    if (env_count < MAX_ENV) {
        snprintf(env_vars[env_count].key, sizeof(env_vars[env_count].key), "%s", key);
        snprintf(env_vars[env_count].val, sizeof(env_vars[env_count].val), "%s", val);
        env_count++;
        setenv(key, val, 1);
    }
}

/**
 * Expand %VARIABLE% references in a string.
 * Also expands %ERRORLEVEL%.
 */
static void expand_vars(const char *in, char *out, size_t out_size,
                        const char *batch_args[]) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_size; ) {
        if (in[i] == '%') {
            /* Batch positional parameter: %0 – %9 */
            if (batch_args && in[i + 1] >= '0' && in[i + 1] <= '9') {
                int idx = in[i + 1] - '0';
                const char *param = batch_args[idx];
                if (param) {
                    size_t plen = strlen(param);
                    if (j + plen < out_size) {
                        memcpy(out + j, param, plen);
                        j += plen;
                    }
                }
                i += 2;
                continue;
            }
            /* Named variable: %NAME% */
            const char *end = strchr(in + i + 1, '%');
            if (end) {
                size_t name_len = (size_t)(end - (in + i + 1));
                char varname[64];
                if (name_len < sizeof(varname)) {
                    memcpy(varname, in + i + 1, name_len);
                    varname[name_len] = '\0';
                    /* Declare errorlevel_buf at this scope so val stays valid */
                    char errorlevel_buf[16];
                    const char *val = NULL;
                    if (strcasecmp(varname, "ERRORLEVEL") == 0) {
                        snprintf(errorlevel_buf, sizeof(errorlevel_buf),
                                 "%d", last_errorlevel);
                        val = errorlevel_buf;
                    } else {
                        val = env_get(varname);
                    }
                    if (val) {
                        size_t vlen = strlen(val);
                        if (j + vlen < out_size) {
                            memcpy(out + j, val, vlen);
                            j += vlen;
                        }
                    }
                }
                i += name_len + 2; /* skip %NAME% */
                continue;
            }
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
}

/* ── DOS-style path helpers ──────────────────────────────────────────── */

/**
 * Translate a POSIX path to a DOS-style display path.
 * /           → C:\
 * /some/dir   → C:\some\dir
 */
static void posix_to_dos(const char *posix, char *dos, size_t size) {
    if (strcmp(posix, "/") == 0) {
        snprintf(dos, size, "C:\\");
        return;
    }
    snprintf(dos, size, "C:%s", posix);
    for (char *p = dos + 2; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

/**
 * Translate a DOS-style path argument to a POSIX path for syscalls.
 * C:\GAMES\DOOM  → /GAMES/DOOM
 * .\subdir       → ./subdir
 * ..\parent      → ../parent
 */
static void dos_to_posix(const char *dos, char *posix, size_t size) {
    /* Strip drive letter prefix if present (C:, D:, …) */
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;

    /* Empty after drive → root */
    if (*p == '\0') {
        snprintf(posix, size, "/");
        return;
    }

    snprintf(posix, size, "%s", p);
    /* Replace backslashes with forward slashes */
    for (char *q = posix; *q; q++) {
        if (*q == '\\') *q = '/';
    }
}

/* ── Prompt ──────────────────────────────────────────────────────────── */

static void print_prompt(void) {
    const char *custom = env_get("PROMPT");
    if (custom && *custom) {
        /* Minimal PROMPT meta-character support: $P = path, $G = '>' */
        for (const char *p = custom; *p; p++) {
            if (*p == '$' && *(p+1)) {
                p++;
                switch (toupper((unsigned char)*p)) {
                    case 'P': {
                        char cwd[MAX_PATH_LEN], dos[MAX_PATH_LEN];
                        if (getcwd(cwd, sizeof(cwd)))
                            posix_to_dos(cwd, dos, sizeof(dos));
                        else
                            snprintf(dos, sizeof(dos), "C:\\");
                        printf("%s", dos);
                        break;
                    }
                    case 'G': printf(">"); break;
                    case 'N': printf("C"); break;
                    case '_': printf("\n"); break;
                    default:  printf("$%c", *p); break;
                }
            } else {
                putchar(*p);
            }
        }
    } else {
        /* Default: C:\current\path> */
        char cwd[MAX_PATH_LEN], dos[MAX_PATH_LEN];
        if (getcwd(cwd, sizeof(cwd)))
            posix_to_dos(cwd, dos, sizeof(dos));
        else
            snprintf(dos, sizeof(dos), "C:\\");
        printf("%s>", dos);
    }
    fflush(stdout);
}

/* ── Utility helpers ─────────────────────────────────────────────────── */

/** Format a file size as a DOS-style right-aligned string (comma-separated). */
static void format_size(long long sz, char *buf, size_t bsize) {
    if (sz < 0) {
        snprintf(buf, bsize, "         ");
        return;
    }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", sz);
    int len = (int)strlen(tmp);
    int out_pos = 0;
    int comma_at = len % 3;
    if (comma_at == 0) comma_at = 3;
    for (int i = 0; i < len && out_pos < (int)bsize - 1; i++) {
        if (i == comma_at && i > 0) {
            buf[out_pos++] = ',';
            comma_at += 3;
        }
        buf[out_pos++] = tmp[i];
    }
    buf[out_pos] = '\0';
}

/** Format a timestamp into "MM-DD-YYYY  HH:MM" */
static void format_dos_time(time_t t, char *buf, size_t bsize) {
    struct tm *tm = localtime(&t);
    if (!tm || bsize < 18) { snprintf(buf, bsize, "  --  "); return; }
    /* "MM-DD-YYYY  HH:MM" is exactly 17 chars; bsize must be >= 18 */
    int n = snprintf(buf, bsize, "%02d-%02d-%04d  %02d:%02d",
                     tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900,
                     tm->tm_hour, tm->tm_min);
    if (n < 0 || (size_t)n >= bsize) buf[bsize - 1] = '\0';
}

/* ── Built-in: DIR ───────────────────────────────────────────────────── */

static void cmd_dir(const char *path_arg) {
    char posix_path[MAX_PATH_LEN];
    if (!path_arg || !*path_arg) {
        if (!getcwd(posix_path, sizeof(posix_path)))
            snprintf(posix_path, sizeof(posix_path), ".");
    } else {
        dos_to_posix(path_arg, posix_path, sizeof(posix_path));
    }

    char dos_path[MAX_PATH_LEN];
    posix_to_dos(posix_path, dos_path, sizeof(dos_path));
    printf(" Directory of %s\n\n", dos_path);

    DIR *d = opendir(posix_path);
    if (!d) {
        fprintf(stderr, "File not found - %s\n", path_arg ? path_arg : ".");
        return;
    }

    long long total_bytes = 0;
    int file_count = 0;
    int dir_count  = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        char full[MAX_PATH_LEN + 256];
        snprintf(full, sizeof(full), "%s/%s", posix_path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        char date_str[32];
        format_dos_time(st.st_mtime, date_str, sizeof(date_str));

        if (S_ISDIR(st.st_mode)) {
            printf("%s    <DIR>          %s\n", date_str, ent->d_name);
            dir_count++;
        } else {
            char size_str[24];
            format_size(st.st_size, size_str, sizeof(size_str));
            printf("%s    %15s %s\n", date_str, size_str, ent->d_name);
            total_bytes += st.st_size;
            file_count++;
        }
    }
    closedir(d);

    char total_str[24];
    format_size(total_bytes, total_str, sizeof(total_str));
    printf("\n");
    printf("         %d File(s)  %s bytes\n", file_count, total_str);
    printf("         %d Dir(s)\n", dir_count);
}

/* ── Built-in: TYPE ──────────────────────────────────────────────────── */

static void cmd_type(const char *path_arg) {
    if (!path_arg || !*path_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char posix_path[MAX_PATH_LEN];
    dos_to_posix(path_arg, posix_path, sizeof(posix_path));

    FILE *f = fopen(posix_path, "r");
    if (!f) {
        fprintf(stderr, "File not found - %s\n", path_arg);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    fclose(f);
}

/* ── Built-in: COPY ──────────────────────────────────────────────────── */

static void cmd_copy(const char *src_arg, const char *dst_arg) {
    if (!src_arg || !dst_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    dos_to_posix(src_arg, src, sizeof(src));
    dos_to_posix(dst_arg, dst, sizeof(dst));

    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "File not found - %s\n", src_arg); return; }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "Access denied - %s\n", dst_arg);
        fclose(in);
        return;
    }

    char buf[65536];
    size_t n;
    long long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
        total += (long long)n;
    }
    fclose(in);
    fclose(out);
    printf("        1 file(s) copied.\n");
    (void)total;
}

/* ── Built-in: DEL ───────────────────────────────────────────────────── */

static void cmd_del(const char *path_arg) {
    if (!path_arg || !*path_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char posix[MAX_PATH_LEN];
    dos_to_posix(path_arg, posix, sizeof(posix));
    if (remove(posix) != 0) {
        fprintf(stderr, "Could not find - %s\n", path_arg);
    }
}

/* ── Built-in: REN ───────────────────────────────────────────────────── */

static void cmd_ren(const char *src_arg, const char *dst_arg) {
    if (!src_arg || !dst_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    dos_to_posix(src_arg, src, sizeof(src));
    dos_to_posix(dst_arg, dst, sizeof(dst));
    if (rename(src, dst) != 0) {
        perror("REN");
    }
}

/* ── Built-in: MKDIR / RMDIR ─────────────────────────────────────────── */

static void cmd_mkdir(const char *path_arg) {
    if (!path_arg || !*path_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char posix[MAX_PATH_LEN];
    dos_to_posix(path_arg, posix, sizeof(posix));
    if (mkdir(posix, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Unable to create directory - %s\n", path_arg);
    }
}

static void cmd_rmdir(const char *path_arg) {
    if (!path_arg || !*path_arg) {
        fprintf(stderr, "Required parameter missing\n");
        return;
    }
    char posix[MAX_PATH_LEN];
    dos_to_posix(path_arg, posix, sizeof(posix));
    if (rmdir(posix) != 0) {
        fprintf(stderr, "Invalid path, not directory, or directory not empty.\n");
    }
}

/* ── Built-in: SET ───────────────────────────────────────────────────── */

static void cmd_set(const char *arg) {
    if (!arg || !*arg) {
        /* Print all variables */
        for (int i = 0; i < env_count; i++) {
            printf("%s=%s\n", env_vars[i].key, env_vars[i].val);
        }
        return;
    }
    char buf[MAX_CMD_LEN];
    snprintf(buf, sizeof(buf), "%s", arg);
    char *eq = strchr(buf, '=');
    if (!eq) {
        /* Show single variable */
        const char *val = env_get(buf);
        if (val) printf("%s=%s\n", buf, val);
        else fprintf(stderr, "Environment variable %s not defined\n", buf);
        return;
    }
    *eq = '\0';
    env_set(buf, eq + 1);
}

/* ── Built-in: ALIAS ─────────────────────────────────────────────────── */

static void cmd_alias(const char *arg) {
    if (!arg || !*arg) {
        for (int i = 0; i < alias_count; i++)
            printf("%s=%s\n", aliases[i].name, aliases[i].expansion);
        return;
    }
    char buf[MAX_CMD_LEN];
    snprintf(buf, sizeof(buf), "%s", arg);
    char *eq = strchr(buf, '=');
    if (!eq) {
        /* Show single alias */
        for (int i = 0; i < alias_count; i++) {
            if (strcasecmp(aliases[i].name, buf) == 0) {
                printf("%s=%s\n", aliases[i].name, aliases[i].expansion);
                return;
            }
        }
        fprintf(stderr, "Alias %s not defined\n", buf);
        return;
    }
    *eq = '\0';
    /* Alias names are limited; copy at most name-field size */
    char alias_name[64];
    memset(alias_name, 0, sizeof(alias_name));
    /* Use memcpy with capped length to avoid strncpy truncation warning */
    size_t nlen = strlen(buf);
    if (nlen >= sizeof(alias_name)) nlen = sizeof(alias_name) - 1;
    memcpy(alias_name, buf, nlen);
    for (int i = 0; i < alias_count; i++) {
        if (strcasecmp(aliases[i].name, alias_name) == 0) {
            snprintf(aliases[i].expansion, sizeof(aliases[i].expansion),
                     "%s", eq + 1);
            return;
        }
    }
    if (alias_count < MAX_ALIASES) {
        memcpy(aliases[alias_count].name, alias_name,
               sizeof(aliases[alias_count].name));
        snprintf(aliases[alias_count].expansion,
                 sizeof(aliases[alias_count].expansion), "%s", eq + 1);
        alias_count++;
    }
}

/* ── Built-in: DATE / TIME / VER ─────────────────────────────────────── */

static void cmd_ver(void) {
    printf("\n%s\n\n", VERSION_STR);
}

static void cmd_date(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    printf("Current date is %s %02d-%02d-%04d\n",
           days[tm->tm_wday],
           tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
}

static void cmd_time_show(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    printf("Current time is %02d:%02d:%02d.%02d\n",
           tm->tm_hour, tm->tm_min, tm->tm_sec, 0);
}

/* ── Built-in: CLS ───────────────────────────────────────────────────── */

static void cmd_cls(void) {
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
}

/* ── Built-in: HISTORY ───────────────────────────────────────────────── */

static void cmd_doskey(void) {
    printf("DOSKEY macro/history listing:\n");
    for (int i = 0; i < history_count; i++)
        printf("  %3d  %s\n", i + 1, history[i]);
}

/* ── Built-in: COMPAT / RUN / LAYERS ────────────────────────────────── */

static void cmd_compat(const char *path) {
    FusionBinInfo info;
    if (!path || !*path) {
        fprintf(stderr, "COMPAT: required parameter missing\n");
        return;
    }
    char posix[MAX_PATH_LEN];
    dos_to_posix(path, posix, sizeof(posix));

    if (fusion_detect_format(posix, &info) != 0) {
        printf("Could not detect binary format for: %s\n", path);
        return;
    }
    printf("Binary   : %s\n", path);
    printf("Format   : ");
    switch (info.fmt) {
        case BIN_ELF:       printf("ELF (Linux)"); break;
        case BIN_PE:        printf("PE (Windows)"); break;
        case BIN_MACHO:     printf("Mach-O (macOS)"); break;
        case BIN_MACHO_FAT: printf("Mach-O Universal"); break;
        default:            printf("Unknown"); break;
    }
    printf("\nArch     : ");
    switch (info.arch) {
        case ARCH_X86:    printf("x86 (32-bit)"); break;
        case ARCH_X86_64: printf("x86-64"); break;
        case ARCH_ARM:    printf("ARM"); break;
        case ARCH_ARM64:  printf("ARM64"); break;
        case ARCH_RISCV:
        case ARCH_RISCV64: printf("RISC-V"); break;
        default:          printf("Unknown"); break;
    }
    printf("\nBits     : %d\n", info.bits);
    printf("Launcher : %s\n",
           info.launcher ? info.launcher : "(native)");
    printf("Desc     : %s\n", info.description);
}

static void cmd_layers(void) {
    printf("FusionOS Active Compatibility Layers:\n");
    printf("  Native ELF x86-64  - direct kernel execution\n");
    printf("  Wine               - PE x86/x86-64 Windows binaries\n");
    printf("  Box86/Box64        - ARM Linux binaries\n");
    printf("  FEX-emu            - ARM64 Linux binaries\n");
    printf("  Darling            - Mach-O macOS binaries (experimental)\n");
}

/* ── HELP ────────────────────────────────────────────────────────────── */

static void cmd_help(const char *topic) {
    if (!topic || !*topic) {
        printf("\nFusionOS COMMAND.COM -- Built-in commands:\n\n");
        printf("  DIR    [path]             List directory contents\n");
        printf("  CD     [path]             Change current directory\n");
        printf("  MD     <path>             Create a directory\n");
        printf("  RD     <path>             Remove a directory\n");
        printf("  COPY   <src> <dst>        Copy a file\n");
        printf("  DEL    <file>             Delete a file\n");
        printf("  REN    <src> <dst>        Rename a file\n");
        printf("  TYPE   <file>             Display file contents\n");
        printf("  CLS                       Clear the screen\n");
        printf("  SET    [var[=val]]         Show / set environment variables\n");
        printf("  PATH   [value]            Show / set PATH\n");
        printf("  ECHO   [text | ON | OFF]  Display text or toggle echo\n");
        printf("  PROMPT [string]           Change the command prompt\n");
        printf("  ALIAS  [name[=expansion]] Show / define aliases\n");
        printf("  DOSKEY                    Show command history\n");
        printf("  DATE                      Show current date\n");
        printf("  TIME                      Show current time\n");
        printf("  VER                       Show OS version\n");
        printf("  GOTO   <label>            Jump to a label in a batch file\n");
        printf("  CALL   <batch> [args]     Call a batch file\n");
        printf("  IF     [NOT] condition    Conditional execution\n");
        printf("  FOR    %%V IN (set) DO cmd Loop over a set\n");
        printf("  REM    [comment]          Remark (ignored)\n");
        printf("  EXIT                      Exit COMMAND.COM\n");
        printf("\n  Multitasking:\n");
        printf("  <cmd> &                   Run command in background\n");
        printf("  JOBS                      List background jobs\n");
        printf("  KILL   [sig] <pid|%%job>  Send signal to a job (default SIGTERM)\n");
        printf("  PAUSE  [%%job|pid]        Suspend job (no arg: wait for keypress)\n");
        printf("  RESUME <%%job|pid>        Resume a suspended job\n");
        printf("  REALTIME <cmd>            Run command at REALTIME priority\n");
        printf("  HIGH     <cmd>            Run command at HIGH priority\n");
        printf("  NORMAL   <cmd>            Run command at NORMAL priority\n");
        printf("\n  Networking:\n");
        printf("  NET STATUS                List network interfaces\n");
        printf("  NET PING   <host>         Test reachability\n");
        printf("  NET CONNECT <host> <port> Test TCP connection\n");
        printf("\n  Filesystem:\n");
        printf("  CHKDSK [device] [part]    Check FAT filesystem integrity\n");
        printf("\n  Modern extensions:\n");
        printf("  INSTALL <pkg|file.fpkg>   Install package or .fpkg file\n");
        printf("  COMPAT  <file>            Detect binary format\n");
        printf("  RUN     <prog> [args]     Run with auto compat-layer\n");
        printf("  LAYERS                    Show active compat layers\n");
        printf("  SYSINFO                   Show system information\n");
        printf("  HELP    [command]         Show help\n");
        printf("\nFor help on a specific command: HELP <command>\n\n");
        return;
    }
    printf("Help for %s not yet available. Type HELP for a command list.\n", topic);
}

/* ── External command executor ───────────────────────────────────────── */

static int run_external(char **args, int argc, int bg, ExecPriority prio) {
    if (!args || !args[0]) return 1;
    (void)argc;

    /* Detect binary format to route through compat layer */
    FusionBinInfo info;
    char posix[MAX_PATH_LEN];
    dos_to_posix(args[0], posix, sizeof(posix));

    int needs_launcher = 0;
    if (fusion_detect_format(posix, &info) == 0) {
        if (info.fmt != BIN_ELF || info.arch != ARCH_X86_64) {
            needs_launcher = 1;
        }
        args[0] = posix;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 127; }

    if (pid == 0) {
        /* ── Child: apply priority, then exec ── */
        if (prio == PRIO_REALTIME) {
            struct sched_param sp;
            sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
            if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
                /* Fallback: requires CAP_SYS_NICE */
                if (setpriority(PRIO_PROCESS, 0, -20) != 0)
                    fprintf(stderr,
                        "Warning: REALTIME priority requires CAP_SYS_NICE; "
                        "running at normal priority.\n");
            }
        } else if (prio == PRIO_HIGH) {
            setpriority(PRIO_PROCESS, 0, -10);
        }

        if (needs_launcher && info.launcher) {
            char *largs[MAX_ARGS];
            int li = 0;
            largs[li++] = (char *)info.launcher;
            for (int i = 0; info.launcher_args[i] && li < MAX_ARGS - 1; i++) {
                if (strcmp(info.launcher_args[i], info.launcher) != 0)
                    largs[li++] = (char *)info.launcher_args[i];
            }
            for (int i = 0; args[i] && li < MAX_ARGS - 1; i++)
                largs[li++] = args[i];
            largs[li] = NULL;
            execv(info.launcher, largs);
        }
        execvp(args[0], args);
        fprintf(stderr, "Bad command or file name - %s\n", args[0]);
        _exit(1);
    }

    if (bg) {
        /* Background: register in job table and return immediately */
        int jid = jobs_add(pid, args[0], prio);
        if (jid > 0) printf("[%d] %d\n", jid, pid);
        return 0;
    }

    /* Foreground: wait for child */
    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) break;
    }
    last_errorlevel = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return last_errorlevel;
}

/* ── Tokeniser ───────────────────────────────────────────────────────── */

static int tokenise(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* ── CALL: forward declaration ────────────────────────────────────────── */

static int exec_batch(const char *path, const char *args[], int depth);

/* ── IF evaluation ───────────────────────────────────────────────────── */

/**
 * Evaluate an IF condition.  Returns 1 if condition is true, 0 if false.
 * Advances *pp past the condition tokens.
 */
static int eval_if_condition(char **argv, int *pos, int argc) {
    int negate = 0;
    if (*pos < argc && strcasecmp(argv[*pos], "NOT") == 0) {
        negate = 1;
        (*pos)++;
    }
    if (*pos >= argc) return negate ? 1 : 0;

    int result = 0;

    /* IF EXIST <file> */
    if (strcasecmp(argv[*pos], "EXIST") == 0) {
        (*pos)++;
        if (*pos < argc) {
            char posix[MAX_PATH_LEN];
            dos_to_posix(argv[(*pos)++], posix, sizeof(posix));
            struct stat st;
            result = (stat(posix, &st) == 0) ? 1 : 0;
        }
    }
    /* IF ERRORLEVEL <n> */
    else if (strcasecmp(argv[*pos], "ERRORLEVEL") == 0) {
        (*pos)++;
        if (*pos < argc) {
            int level = atoi(argv[(*pos)++]);
            result = (last_errorlevel >= level) ? 1 : 0;
        }
    }
    /* IF "str1"=="str2"  (supports "lhs"=="rhs", lhs==rhs, and lhs == rhs) */
    else {
        const char *lhs = argv[(*pos)++];
        if (*pos >= argc) goto done_if;

        const char *op = argv[*pos];
        const char *rhs = NULL;
        /*
         * lhs_buf and rhs_buf are declared here (enclosing scope) so that
         * 'lhs' and 'rhs' remain valid when strcmp() is called below,
         * regardless of which parsing branch is taken.
         */
        char lhs_buf[MAX_CMD_LEN];
        char rhs_buf[MAX_CMD_LEN];

        if (strcmp(op, "==") == 0) {
            /* Standard DOS spacing: lhs == rhs */
            (*pos)++;
            if (*pos < argc) rhs = argv[(*pos)++];
        } else if (strncmp(op, "==", 2) == 0) {
            /* Embedded: =="rhs" — no space before RHS */
            (*pos)++;
            const char *embedded = op + 2;
            /* Strip surrounding quotes if present */
            if (embedded[0] == '"') {
                size_t elen = strlen(embedded + 1);
                snprintf(rhs_buf, sizeof(rhs_buf), "%s", embedded + 1);
                if (elen > 0 && rhs_buf[elen - 1] == '"')
                    rhs_buf[elen - 1] = '\0';
                rhs = rhs_buf;
            } else {
                rhs = embedded;
            }
        } else {
            /* Try lhs==rhs embedded in the lhs token itself */
            const char *eq = strstr(lhs, "==");
            if (eq) {
                size_t llen = (size_t)(eq - lhs);
                size_t copy_len = llen < sizeof(lhs_buf) - 1
                                  ? llen : sizeof(lhs_buf) - 1;
                memcpy(lhs_buf, lhs, copy_len);
                lhs_buf[copy_len] = '\0';
                lhs = lhs_buf;  /* lhs_buf is in the enclosing else{} scope */
                const char *rhs_start = eq + 2;
                if (rhs_start[0] == '"') {
                    size_t rlen = strlen(rhs_start + 1);
                    snprintf(rhs_buf, sizeof(rhs_buf), "%s", rhs_start + 1);
                    if (rlen > 0 && rhs_buf[rlen - 1] == '"')
                        rhs_buf[rlen - 1] = '\0';
                    rhs = rhs_buf;
                } else {
                    rhs = rhs_start;
                }
            }
        }

        if (rhs) result = (strcmp(lhs, rhs) == 0) ? 1 : 0;
        done_if:;
    }

    return negate ? !result : result;
}

/* ── Core command dispatcher ─────────────────────────────────────────── */

/**
 * Execute one (already-expanded, tokenised) command.
 * Returns an exit-code integer.
 */
static int dispatch(char **argv, int argc, int batch_depth) {
    if (argc == 0 || !argv[0]) return 0;

    /* ── Detect background operator (&) ────────────────────────────── */
    int bg = 0;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
        bg = 1;
        argc--;
        argv[argc] = NULL;
        if (argc == 0) return 0;
    }

    /* ── Detect priority prefix ─────────────────────────────────────── */
    ExecPriority prio = PRIO_NORMAL;
    const char *cmd = argv[0];
    if (strcasecmp(cmd, "REALTIME") == 0) {
        prio = PRIO_REALTIME;
        argv++; argc--;
        if (argc == 0) { fprintf(stderr, "REALTIME: missing command\n"); return 1; }
        cmd = argv[0];
    } else if (strcasecmp(cmd, "HIGH") == 0) {
        prio = PRIO_HIGH;
        argv++; argc--;
        if (argc == 0) { fprintf(stderr, "HIGH: missing command\n"); return 1; }
        cmd = argv[0];
    } else if (strcasecmp(cmd, "NORMAL") == 0) {
        prio = PRIO_NORMAL;
        argv++; argc--;
        if (argc == 0) { fprintf(stderr, "NORMAL: missing command\n"); return 1; }
        cmd = argv[0];
    }

    /* ── Internal commands ─────────────────────────────── */

    if (strcasecmp(cmd, "REM") == 0 || cmd[0] == ':')
        return 0; /* comment / label */

    if (strcasecmp(cmd, "ECHO") == 0) {
        if (argc == 1) {
            printf("ECHO is %s\n", echo_on ? "on" : "off");
            return 0;
        }
        if (strcasecmp(argv[1], "ON") == 0)  { echo_on = 1; return 0; }
        if (strcasecmp(argv[1], "OFF") == 0) { echo_on = 0; return 0; }
        /* Print remaining tokens */
        for (int i = 1; i < argc; i++) {
            if (i > 1) putchar(' ');
            printf("%s", argv[i]);
        }
        putchar('\n');
        return 0;
    }

    if (strcasecmp(cmd, "CLS") == 0)   { cmd_cls(); return 0; }
    if (strcasecmp(cmd, "VER") == 0)   { cmd_ver(); return 0; }
    if (strcasecmp(cmd, "DATE") == 0)  { cmd_date(); return 0; }
    if (strcasecmp(cmd, "TIME") == 0)  { cmd_time_show(); return 0; }

    if (strcasecmp(cmd, "EXIT") == 0) exit(0);

    if (strcasecmp(cmd, "DIR") == 0) {
        cmd_dir(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "CD") == 0 || strcasecmp(cmd, "CHDIR") == 0) {
        if (argc < 2) {
            char cwd[MAX_PATH_LEN], dos[MAX_PATH_LEN];
            if (getcwd(cwd, sizeof(cwd))) {
                posix_to_dos(cwd, dos, sizeof(dos));
                printf("%s\n", dos);
            }
            return 0;
        }
        char posix[MAX_PATH_LEN];
        dos_to_posix(argv[1], posix, sizeof(posix));
        if (chdir(posix) != 0) {
            fprintf(stderr, "The system cannot find the path specified.\n");
            return 1;
        }
        return 0;
    }

    if (strcasecmp(cmd, "MD") == 0 || strcasecmp(cmd, "MKDIR") == 0) {
        cmd_mkdir(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "RD") == 0 || strcasecmp(cmd, "RMDIR") == 0) {
        cmd_rmdir(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "TYPE") == 0) {
        cmd_type(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "COPY") == 0) {
        cmd_copy(argc > 1 ? argv[1] : NULL,
                 argc > 2 ? argv[2] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "DEL") == 0 || strcasecmp(cmd, "ERASE") == 0) {
        cmd_del(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "REN") == 0 || strcasecmp(cmd, "RENAME") == 0) {
        cmd_ren(argc > 1 ? argv[1] : NULL,
                argc > 2 ? argv[2] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "SET") == 0) {
        /* Rejoin remaining tokens to restore "key=value with spaces" */
        if (argc > 1) {
            char joined[MAX_CMD_LEN];
            snprintf(joined, sizeof(joined), "%s", argv[1]);
            for (int i = 2; i < argc; i++) {
                strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
                strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 1);
            }
            cmd_set(joined);
        } else {
            cmd_set(NULL);
        }
        return 0;
    }

    if (strcasecmp(cmd, "PATH") == 0) {
        if (argc < 2) {
            const char *p = env_get("PATH");
            printf("PATH=%s\n", p ? p : "");
        } else {
            env_set("PATH", argv[1]);
        }
        return 0;
    }

    if (strcasecmp(cmd, "PROMPT") == 0) {
        if (argc < 2) env_set("PROMPT", "$P$G");
        else          env_set("PROMPT", argv[1]);
        return 0;
    }

    if (strcasecmp(cmd, "ALIAS") == 0) {
        if (argc > 1) {
            char joined[MAX_CMD_LEN];
            snprintf(joined, sizeof(joined), "%s", argv[1]);
            for (int i = 2; i < argc; i++) {
                strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
                strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 1);
            }
            cmd_alias(joined);
        } else {
            cmd_alias(NULL);
        }
        return 0;
    }

    if (strcasecmp(cmd, "DOSKEY") == 0) { cmd_doskey(); return 0; }

    if (strcasecmp(cmd, "HELP") == 0) {
        cmd_help(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    /* IF command */
    if (strcasecmp(cmd, "IF") == 0) {
        int pos = 1;
        int cond = eval_if_condition(argv, &pos, argc);
        if (cond && pos < argc) {
            /* Execute remainder as a command */
            return dispatch(argv + pos, argc - pos, batch_depth);
        }
        return 0;
    }

    /* CALL <batchfile> [args] */
    if (strcasecmp(cmd, "CALL") == 0) {
        if (argc < 2) { fprintf(stderr, "CALL: missing batch file\n"); return 1; }
        char posix[MAX_PATH_LEN];
        dos_to_posix(argv[1], posix, sizeof(posix));
        const char *bargs[MAX_ARGS];
        bargs[0] = argv[1];
        for (int i = 2; i < argc && i < MAX_ARGS - 1; i++)
            bargs[i - 1] = argv[i];
        bargs[argc - 1] = NULL;
        return exec_batch(posix, bargs, batch_depth + 1);
    }

    /* Modern extensions */
    if (strcasecmp(cmd, "COMPAT") == 0) {
        cmd_compat(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "LAYERS") == 0) { cmd_layers(); return 0; }

    if (strcasecmp(cmd, "INSTALL") == 0) {
        argv[0] = "fusion-pkg";
        return run_external(argv, argc, bg, prio);
    }

    if (strcasecmp(cmd, "RUN") == 0) {
        if (argc < 2) { fprintf(stderr, "RUN: missing program\n"); return 1; }
        return run_external(argv + 1, argc - 1, bg, prio);
    }

    if (strcasecmp(cmd, "SYSINFO") == 0) {
        argv[0] = "fusion-monitor";
        argv[1] = NULL;
        return run_external(argv, 1, 0, PRIO_NORMAL);
    }

    /* ── Job control ─────────────────────────────────────────────────── */

    if (strcasecmp(cmd, "JOBS") == 0) { cmd_jobs(); return 0; }

    if (strcasecmp(cmd, "KILL") == 0) {
        /* KILL [signal] <pid|%job>  — default signal: SIGTERM */
        int sig = SIGTERM;
        const char *spec = NULL;
        if (argc == 2) {
            spec = argv[1];
        } else if (argc == 3) {
            sig = atoi(argv[1]);
            spec = argv[2];
        }
        cmd_kill_job(spec, sig > 0 ? sig : SIGTERM);
        return 0;
    }

    if (strcasecmp(cmd, "PAUSE") == 0) {
        /* PAUSE [%job|pid] — with no job arg: interactive "press any key" */
        cmd_pause_job(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    if (strcasecmp(cmd, "RESUME") == 0) {
        cmd_resume_job(argc > 1 ? argv[1] : NULL);
        return 0;
    }

    /* ── Networking ──────────────────────────────────────────────────── */

    if (strcasecmp(cmd, "NET") == 0) {
        /* Pass all args through to fusion-net: NET STATUS → fusion-net STATUS */
        argv[0] = "fusion-net";
        return run_external(argv, argc, 0, PRIO_NORMAL);
    }

    /* ── FAT integrity ───────────────────────────────────────────────── */

    if (strcasecmp(cmd, "CHKDSK") == 0) {
        argv[0] = "fat-chkdsk";
        return run_external(argv, argc, 0, PRIO_NORMAL);
    }

    /* Check if alias matches */
    for (int i = 0; i < alias_count; i++) {
        if (strcasecmp(aliases[i].name, cmd) == 0) {
            char expanded[MAX_CMD_LEN];
            /* Append original args after alias expansion */
            snprintf(expanded, sizeof(expanded), "%s", aliases[i].expansion);
            for (int j = 1; j < argc; j++) {
                strncat(expanded, " ", sizeof(expanded) - strlen(expanded) - 1);
                strncat(expanded, argv[j], sizeof(expanded) - strlen(expanded) - 1);
            }
            char exp_buf[MAX_CMD_LEN];
            expand_vars(expanded, exp_buf, sizeof(exp_buf), NULL);
            char *new_argv[MAX_ARGS];
            int new_argc = tokenise(exp_buf, new_argv, MAX_ARGS);
            return dispatch(new_argv, new_argc, batch_depth);
        }
    }

    /* External command / program */
    return run_external(argv, argc, bg, prio);
}

/* ── Batch (.BAT) executor ───────────────────────────────────────────── */

static int exec_batch(const char *path, const char *args[], int depth) {
    if (depth >= MAX_BATCH_DEPTH) {
        fprintf(stderr, "Batch nesting too deep\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open batch file: %s\n", path);
        return 1;
    }

    /* Load all lines */
#define BATCH_MAX_LINES 4096
    char (*lines)[MAX_CMD_LEN] = malloc(BATCH_MAX_LINES * MAX_CMD_LEN);
    if (!lines) { fclose(f); return 1; }

    int line_count = 0;
    while (line_count < BATCH_MAX_LINES &&
           fgets(lines[line_count], MAX_CMD_LEN, f)) {
        /* Strip trailing newline */
        size_t len = strlen(lines[line_count]);
        while (len > 0 && (lines[line_count][len-1] == '\n' ||
                            lines[line_count][len-1] == '\r'))
            lines[line_count][--len] = '\0';
        line_count++;
    }
    fclose(f);

    int ip = 0; /* instruction pointer */
    int rc = 0;

    while (ip < line_count) {
        char *raw = lines[ip++];

        /* Skip empty lines */
        const char *p = raw;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        /* Labels: :LABELNAME */
        if (*p == ':') continue;

        /* Suppress echo if OFF */
        int at_prefix = (*p == '@');
        if (at_prefix) p++;

        /* Strip '@' prefix for processing */
        char expanded[MAX_CMD_LEN];
        expand_vars(p, expanded, sizeof(expanded), args);

        if (echo_on && !at_prefix) {
            print_prompt();
            printf("%s\n", expanded);
        }

        /* Handle GOTO inline so we can jump lines */
        char tmp[MAX_CMD_LEN];
        snprintf(tmp, sizeof(tmp), "%s", expanded);
        char *av[MAX_ARGS];
        int ac = tokenise(tmp, av, MAX_ARGS);
        if (ac == 0) continue;

        if (strcasecmp(av[0], "GOTO") == 0 && ac > 1) {
            const char *target = av[1];
            /* Scan for :LABEL */
            for (int li = 0; li < line_count; li++) {
                const char *lp = lines[li];
                while (*lp == ' ' || *lp == '\t') lp++;
                if (*lp == ':') {
                    lp++;
                    while (*lp == ' ' || *lp == '\t') lp++;
                    /* Compare label name case-insensitively */
                    if (strcasecmp(lp, target) == 0 ||
                        (strncasecmp(lp, target, strlen(target)) == 0 &&
                         (lp[strlen(target)] == '\0' ||
                          lp[strlen(target)] == ' '))) {
                        ip = li + 1;
                        goto next_line;
                    }
                }
            }
            fprintf(stderr, "Label not found - %s\n", target);
            rc = 1;
            goto next_line;
        }

        rc = dispatch(av, ac, depth);
        next_line:;
    }

    free(lines);
    return rc;
}

/* ── CONFIG.SYS parser ───────────────────────────────────────────────── */

static void parse_config_sys(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[MAX_CMD_LEN];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == ';' || *p == '#') continue;  /* comment */

        /* FILES=<n>, BUFFERS=<n>, BREAK=ON/OFF */
        if (strncasecmp(p, "FILES=", 6) == 0) {
            /* Informational in hosted mode */
        } else if (strncasecmp(p, "BUFFERS=", 8) == 0) {
            /* Informational in hosted mode */
        } else if (strncasecmp(p, "BREAK=", 6) == 0) {
            /* Could set sigint behaviour */
        } else if (strncasecmp(p, "SET ", 4) == 0) {
            char joined[MAX_CMD_LEN];
            snprintf(joined, sizeof(joined), "%s", p + 4);
            cmd_set(joined);
        }
    }
    fclose(f);
}

/* ── AUTOEXEC.BAT runner ─────────────────────────────────────────────── */

static void run_autoexec(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    const char *args[1] = { NULL };
    exec_batch(path, args, 0);
}

/* ── Main read-execute loop ──────────────────────────────────────────── */

static int read_line(char *line, size_t size) {
    /* fgets takes int; cap to INT_MAX to prevent truncation on huge buffers */
    int isize = (size > (size_t)INT_MAX) ? INT_MAX : (int)size;
    if (!fgets(line, isize, stdin)) return -1;
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';
    return 0;
}

int main(int argc, char *argv[]) {
    /* Initialise default environment */
    env_set("COMSPEC",  "C:\\COMMAND.COM");
    env_set("OS",       "FusionOS");
    env_set("FUSIONOS", "1");

    const char *path_env = getenv("PATH");
    if (path_env) env_set("PATH", path_env);
    else          env_set("PATH", "C:\\BIN;C:\\DOS;C:\\GAMES");

    /* Process CONFIG.SYS */
    parse_config_sys("/etc/fusionos/config.sys");
    parse_config_sys("C:\\CONFIG.SYS");  /* DOS-style fallback */

    /* If a script was passed on the command line, execute it */
    if (argc > 1) {
        const char *bargs[MAX_ARGS];
        bargs[0] = argv[1];
        for (int i = 2; i < argc && i < MAX_ARGS - 1; i++)
            bargs[i - 1] = argv[i];
        bargs[argc > 1 ? argc - 1 : 0] = NULL;
        int rc = exec_batch(argv[1], bargs, 0);
        return rc;
    }

    /* Interactive session */
    int interactive = isatty(STDIN_FILENO);

    if (interactive) {
        cmd_ver();
        printf("Type HELP for a list of commands.\n\n");
    }

    /* Run AUTOEXEC.BAT */
    if (interactive) {
        run_autoexec("/etc/fusionos/autoexec.bat");
        run_autoexec("C:\\AUTOEXEC.BAT");
    }

    char line[MAX_CMD_LEN];
    while (1) {
        if (interactive) {
            reap_jobs();   /* non-blocking: notify finished background jobs */
            print_prompt();
        }

        if (read_line(line, sizeof(line)) != 0) break;

        if (!line[0]) continue;

        /* Add to history */
        if (history_count < HISTORY_SIZE)
            snprintf(history[history_count++], MAX_CMD_LEN, "%s", line);

        /* Expand variables */
        char expanded[MAX_CMD_LEN];
        expand_vars(line, expanded, sizeof(expanded), NULL);

        /* Tokenise */
        char *args[MAX_ARGS];
        int ac = tokenise(expanded, args, MAX_ARGS);
        if (ac == 0) continue;

        last_errorlevel = dispatch(args, ac, 0);
    }

    return last_errorlevel;
}
