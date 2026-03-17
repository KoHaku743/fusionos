/* fusion-monitor: FusionOS system monitor TUI
 * Reads /proc/stat, /proc/meminfo, /proc/[pid]/stat and /proc/[pid]/cmdline.
 * Uses ANSI escape codes only — no ncurses dependency.
 * Refreshes every 1 second; handles SIGWINCH for terminal resize.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>

/* ─── ANSI helpers ─────────────────────────────────── */
#define ANSI_RESET    "\033[0m"
#define ANSI_BOLD     "\033[1m"
#define ANSI_RED      "\033[31m"
#define ANSI_GREEN    "\033[32m"
#define ANSI_YELLOW   "\033[33m"
#define ANSI_BLUE     "\033[34m"
#define ANSI_CYAN     "\033[36m"
#define ANSI_WHITE    "\033[37m"
#define ANSI_BG_BLUE  "\033[44m"
#define ANSI_CLEAR    "\033[2J"
#define ANSI_HOME     "\033[H"
#define ANSI_ERASE_LINE "\033[K"

#define MAX_CPUS      64
#define MAX_PROCS     4096
#define TOP_N         10

/* ─── Terminal size (updated on SIGWINCH) ──────────── */
static volatile sig_atomic_t g_winch = 0;
static int g_rows = 24;
static int g_cols = 80;

/* ─── CPU sample (one per logical CPU) ─────────────── */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CpuSample;

static int       g_ncpus = 0;

/* ─── Per-process info ──────────────────────────────── */
typedef struct {
    int    pid;
    char   comm[64];
    char   state;
    unsigned long long utime;   /* from /proc/pid/stat field 14 */
    unsigned long long stime;   /* field 15 */
    unsigned long long total_prev;
    unsigned long long total_cur;
    double cpu_pct;
    long   vmrss_kb;
} ProcInfo;

static ProcInfo g_procs[MAX_PROCS];
static int      g_nprocs = 0;

/* ─── Compat layer counts ───────────────────────────── */
typedef struct {
    const char *name;
    int         count;
} CompatLayer;

static CompatLayer g_layers[] = {
    { "wine",      0 },
    { "darling",   0 },
    { "box64",     0 },
    { "FEXLoader", 0 },
    { NULL, 0 }
};

/* ════════════════════════════════════════════════════
 * SIGWINCH handler — just set flag, query size on next draw
 * ═════════════════════════════════════════════════════ */
static void handle_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

/* ════════════════════════════════════════════════════
 * update_winsize — call after SIGWINCH or on start
 * ═════════════════════════════════════════════════════ */
static void update_winsize(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col;
    }
}

/* ════════════════════════════════════════════════════
 * safe_write — write with EINTR retry
 * ═════════════════════════════════════════════════════ */
static void safe_write(const char *s, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t r = write(STDOUT_FILENO, s + written, len - written);
        if (r == -1) {
            if (errno == EINTR) continue;
            break;
        }
        written += (size_t)r;
    }
}

/* printf-style helper that writes to stdout */
static void tui_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tui_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) safe_write(buf, (size_t)n);
}

/* ════════════════════════════════════════════════════
 * read_file_into_buf — open file, read up to bufsz-1 bytes, null-terminate
 * Returns bytes read, or -1 on error.
 * ═════════════════════════════════════════════════════ */
static int read_file_into_buf(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;

    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t r = read(fd, buf + total, bufsz - 1 - total);
        if (r == -1) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    close(fd);
    return (int)total;
}

/* ════════════════════════════════════════════════════
 * parse_cpu_sample — parse one "cpu..." line from /proc/stat
 * ═════════════════════════════════════════════════════ */
static void parse_cpu_sample(const char *line, CpuSample *s) {
    /* skip the "cpu" / "cpuN" prefix */
    while (*line && !isspace((unsigned char)*line)) line++;
    sscanf(line, " %llu %llu %llu %llu %llu %llu %llu %llu",
           &s->user, &s->nice, &s->system, &s->idle,
           &s->iowait, &s->irq, &s->softirq, &s->steal);
}

/* ════════════════════════════════════════════════════
 * sample_cpus — read /proc/stat and update g_cpu_prev
 * ═════════════════════════════════════════════════════ */
static void sample_cpus(CpuSample *out, int *ncpus_out) {
    char buf[8192];
    if (read_file_into_buf("/proc/stat", buf, sizeof(buf)) < 0) return;

    int idx = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strncmp(line, "cpu", 3) == 0) {
            if (idx < MAX_CPUS + 1) {  /* array is out[MAX_CPUS+1], valid idx: 0..MAX_CPUS */
                parse_cpu_sample(line, &out[idx]);
                idx++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    *ncpus_out = idx - 1;  /* -1 because idx=0 is aggregate "cpu" line */
}

/* ════════════════════════════════════════════════════
 * cpu_pct — compute usage % from two samples
 * ═════════════════════════════════════════════════════ */
static double cpu_pct(const CpuSample *prev, const CpuSample *cur) {
    unsigned long long prev_total = prev->user + prev->nice + prev->system
                                  + prev->idle + prev->iowait + prev->irq
                                  + prev->softirq + prev->steal;
    unsigned long long cur_total  = cur->user  + cur->nice  + cur->system
                                  + cur->idle  + cur->iowait + cur->irq
                                  + cur->softirq + cur->steal;
    unsigned long long delta_total = cur_total - prev_total;
    if (delta_total == 0) return 0.0;

    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long cur_idle  = cur->idle  + cur->iowait;
    unsigned long long delta_idle = cur_idle - prev_idle;

    return 100.0 * (double)(delta_total - delta_idle) / (double)delta_total;
}

/* ════════════════════════════════════════════════════
 * read_meminfo — fill MemTotal, MemAvailable (kB)
 * ═════════════════════════════════════════════════════ */
static void read_meminfo(long *mem_total_kb, long *mem_avail_kb) {
    char buf[4096];
    *mem_total_kb = 0;
    *mem_avail_kb = 0;
    if (read_file_into_buf("/proc/meminfo", buf, sizeof(buf)) < 0) return;

    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        long val = 0;
        if (sscanf(line, "MemTotal: %ld", &val) == 1)      *mem_total_kb = val;
        if (sscanf(line, "MemAvailable: %ld", &val) == 1)  *mem_avail_kb = val;
        line = nl ? nl + 1 : NULL;
    }
}

/* ════════════════════════════════════════════════════
 * parse_ull — parse an unsigned long long from a whitespace-delimited
 * token string, skipping 'skip' tokens first, writing result to *out.
 * Returns pointer to next token, or NULL.
 * ═════════════════════════════════════════════════════ */
static const char *skip_tokens(const char *s, int skip) {
    for (int i = 0; i < skip && s && *s; i++) {
        /* skip non-space */
        while (*s && !isspace((unsigned char)*s)) s++;
        /* skip space */
        while (*s && isspace((unsigned char)*s)) s++;
    }
    return s;
}

static unsigned long long parse_ull_token(const char *s) {
    unsigned long long v = 0;
    while (*s && isspace((unsigned char)*s)) s++;
    while (*s && isdigit((unsigned char)*s)) {
        v = v * 10 + (unsigned long long)(*s - '0');
        s++;
    }
    return v;
}

/* ════════════════════════════════════════════════════
 * scan_procs — read all /proc/[pid]/stat entries
 * Updates g_procs[] array.
 * ═════════════════════════════════════════════════════ */
static void scan_procs(void) {
    DIR *d = opendir("/proc");
    if (!d) return;

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Ensure bounds before writing */
        if (n >= MAX_PROCS) break;

        /* Only numeric entries are PIDs */
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        char path[32];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);

        char buf[512];
        if (read_file_into_buf(path, buf, sizeof(buf)) <= 0) continue;

        ProcInfo *p = &g_procs[n];
        memset(p, 0, sizeof(*p));
        p->pid = pid;

        /* Format: pid (comm) state ppid ... utime stime ... rss ... */
        char *open_paren = strchr(buf, '(');
        char *close_paren = strrchr(buf, ')');
        if (!open_paren || !close_paren) continue;

        /* Extract comm */
        size_t comm_len = (size_t)(close_paren - open_paren - 1);
        if (comm_len >= sizeof(p->comm)) comm_len = sizeof(p->comm) - 1;
        memcpy(p->comm, open_paren + 1, comm_len);
        p->comm[comm_len] = '\0';

        /* After ')' the fields (1-based stat numbering after pid+comm) are:
         * 3:state 4:ppid 5:pgrp 6:session 7:tty 8:tpgid 9:flags
         * 10:minflt 11:cminflt 12:majflt 13:cmajflt
         * 14:utime 15:stime 16:cutime 17:cstime 18:priority 19:nice
         * 20:num_threads 21:itrealvalue 22:starttime 23:vsize 24:rss */
        const char *rest = close_paren + 1;
        while (*rest && isspace((unsigned char)*rest)) rest++;

        /* Field 3 (state) — skip 1 token to reach field 4 */
        rest = skip_tokens(rest, 1);   /* now at field 4 (ppid) */
        rest = skip_tokens(rest, 9);   /* skip ppid..cmajflt (fields 4-13) → at 14 */
        p->utime = parse_ull_token(rest);
        rest = skip_tokens(rest, 1);
        p->stime = parse_ull_token(rest);
        rest = skip_tokens(rest, 1);
        /* skip cutime cstime priority nice num_threads itrealvalue starttime vsize (9 fields) */
        rest = skip_tokens(rest, 9);   /* now at field 24 (rss, in pages) */
        long rss_pages = 0;
        sscanf(rest, "%ld", &rss_pages);
        p->vmrss_kb = rss_pages * 4;   /* pages → kB (4 kB pages) */

        p->total_cur = p->utime + p->stime;
        n++;
    }
    closedir(d);
    g_nprocs = n;
}

/* ════════════════════════════════════════════════════
 * compute_proc_cpu — compare current scan with previous totals
 * Needs system-wide CPU delta for percentage calculation.
 * ═════════════════════════════════════════════════════ */
static void compute_proc_cpu(unsigned long long sys_delta) {
    (void)sys_delta;  /* used implicitly via g_nprocs comparison */
    /* For simplicity, we store total_prev from last iteration */
    /* The caller sets total_prev before calling scan_procs */
    for (int i = 0; i < g_nprocs; i++) {
        ProcInfo *p = &g_procs[i];
        /* Look up previous total by PID */
        /* (full implementation would use a hash; here we do linear scan) */
        p->cpu_pct = 0.0;
        if (sys_delta > 0) {
            /* cpu_pct approximation: process delta / system delta * 100 */
            unsigned long long delta = p->total_cur - p->total_prev;
            p->cpu_pct = 100.0 * (double)delta / (double)sys_delta;
        }
    }
}

/* ════════════════════════════════════════════════════
 * scan_compat_layers — count compat layer processes
 * Reads /proc/[pid]/cmdline and looks for known launcher names.
 * ═════════════════════════════════════════════════════ */
static void scan_compat_layers(void) {
    /* Reset counts */
    for (int i = 0; g_layers[i].name; i++) g_layers[i].count = 0;

    DIR *d = opendir("/proc");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        char path[32];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", atoi(ent->d_name));

        char buf[256];
        int n = read_file_into_buf(path, buf, sizeof(buf));
        if (n <= 0) continue;

        /* cmdline uses NUL separators; replace them with spaces for searching */
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\0') buf[i] = ' ';
        }

        for (int i = 0; g_layers[i].name; i++) {
            if (strstr(buf, g_layers[i].name)) {
                g_layers[i].count++;
            }
        }
    }
    closedir(d);
}

/* ════════════════════════════════════════════════════
 * draw_bar — draw a horizontal bar graph [####    ] pct%
 * ═════════════════════════════════════════════════════ */
static void draw_bar(double pct, int width, const char *color) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    int filled = (int)(pct * width / 100.0);
    safe_write("[", 1);
    safe_write(color, strlen(color));
    for (int i = 0; i < width; i++) {
        safe_write(i < filled ? "#" : " ", 1);
    }
    safe_write(ANSI_RESET, strlen(ANSI_RESET));
    safe_write("]", 1);
}

/* ════════════════════════════════════════════════════
 * qsort comparator — sort by cpu_pct descending
 * ═════════════════════════════════════════════════════ */
static int cmp_cpu_desc(const void *a, const void *b) {
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pb->cpu_pct > pa->cpu_pct) return 1;
    if (pb->cpu_pct < pa->cpu_pct) return -1;
    return 0;
}

/* ════════════════════════════════════════════════════
 * draw_header — top status bar
 * ═════════════════════════════════════════════════════ */
static void draw_header(void) {
    /* Get current time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

    /* Print header bar */
    tui_printf(ANSI_BG_BLUE ANSI_BOLD ANSI_WHITE
               " %-*s%s " ANSI_RESET "\n",
               g_cols - 20, " FusionOS Monitor", timebuf);
}

/* ════════════════════════════════════════════════════
 * draw_cpu_section — per-core CPU bars using two snapshots
 * ═════════════════════════════════════════════════════ */
static void draw_cpu_section(CpuSample *prev, CpuSample *cur, int ncpus) {
    tui_printf(ANSI_BOLD ANSI_CYAN "CPU Usage" ANSI_RESET "\n");

    int bar_width = (g_cols > 30) ? (g_cols - 20) : 10;

    /* Aggregate first */
    double pct0 = cpu_pct(&prev[0], &cur[0]);
    tui_printf("  All  %5.1f%% ", pct0);
    draw_bar(pct0, bar_width, pct0 > 80.0 ? ANSI_RED : pct0 > 50.0 ? ANSI_YELLOW : ANSI_GREEN);
    tui_printf(ANSI_ERASE_LINE "\n");

    /* Per-core (up to min(ncpus, g_rows/3)) */
    int show = ncpus;
    if (show > 8) show = 8;
    for (int i = 1; i <= show; i++) {
        double p = cpu_pct(&prev[i], &cur[i]);
        tui_printf("  CPU%-2d %5.1f%% ", i - 1, p);
        draw_bar(p, bar_width, p > 80.0 ? ANSI_RED : p > 50.0 ? ANSI_YELLOW : ANSI_GREEN);
        tui_printf(ANSI_ERASE_LINE "\n");
    }
}

/* ════════════════════════════════════════════════════
 * draw_mem_section — memory bar
 * ═════════════════════════════════════════════════════ */
static void draw_mem_section(void) {
    long total_kb, avail_kb;
    read_meminfo(&total_kb, &avail_kb);

    long used_kb = total_kb - avail_kb;
    double pct = (total_kb > 0) ? (100.0 * (double)used_kb / (double)total_kb) : 0.0;

    tui_printf(ANSI_BOLD ANSI_CYAN "Memory" ANSI_RESET "\n");

    int bar_width = (g_cols > 20) ? (g_cols - 20) : 10;
    tui_printf("  Mem   %5.1f%% ", pct);
    draw_bar(pct, bar_width, pct > 80.0 ? ANSI_RED : pct > 60.0 ? ANSI_YELLOW : ANSI_GREEN);
    tui_printf(ANSI_ERASE_LINE "\n");
    tui_printf("  Used: %ld MiB / %ld MiB" ANSI_ERASE_LINE "\n",
               used_kb / 1024, total_kb / 1024);
}

/* ════════════════════════════════════════════════════
 * draw_compat_section — active compat layers
 * ═════════════════════════════════════════════════════ */
static void draw_compat_section(void) {
    tui_printf(ANSI_BOLD ANSI_CYAN "Compat Layers" ANSI_RESET "\n");

    int any = 0;
    for (int i = 0; g_layers[i].name; i++) {
        if (g_layers[i].count > 0) {
            tui_printf("  %s%-10s%s %d process%s" ANSI_ERASE_LINE "\n",
                       ANSI_GREEN, g_layers[i].name, ANSI_RESET,
                       g_layers[i].count,
                       g_layers[i].count == 1 ? "" : "es");
            any = 1;
        }
    }
    if (!any) {
        tui_printf("  (none active)" ANSI_ERASE_LINE "\n");
    }
}

/* ════════════════════════════════════════════════════
 * draw_procs_section — top N processes by CPU
 * ═════════════════════════════════════════════════════ */
static void draw_procs_section(void) {
    tui_printf(ANSI_BOLD ANSI_CYAN "Top Processes (by CPU)" ANSI_RESET "\n");
    tui_printf("  %s%-6s %-16s %6s %8s%s" ANSI_ERASE_LINE "\n",
               ANSI_BOLD, "PID", "COMMAND", "CPU%", "MEM(MiB)", ANSI_RESET);

    /* Sort a local copy */
    ProcInfo sorted[MAX_PROCS];
    int n = g_nprocs < MAX_PROCS ? g_nprocs : MAX_PROCS;
    memcpy(sorted, g_procs, n * sizeof(ProcInfo));
    qsort(sorted, (size_t)n, sizeof(ProcInfo), cmp_cpu_desc);

    int show = n < TOP_N ? n : TOP_N;
    for (int i = 0; i < show; i++) {
        tui_printf("  %-6d %-16.16s %5.1f%% %7ld" ANSI_ERASE_LINE "\n",
                   sorted[i].pid,
                   sorted[i].comm,
                   sorted[i].cpu_pct,
                   sorted[i].vmrss_kb / 1024);
    }
}

/* ════════════════════════════════════════════════════
 * main — monitor loop
 * ═════════════════════════════════════════════════════ */
int main(void) {
    /* Setup SIGWINCH */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    update_winsize();

    /* First CPU sample */
    CpuSample cpu_a[MAX_CPUS + 1];
    CpuSample cpu_b[MAX_CPUS + 1];
    memset(cpu_a, 0, sizeof(cpu_a));
    memset(cpu_b, 0, sizeof(cpu_b));

    sample_cpus(cpu_a, &g_ncpus);

    /* First process scan to populate total_prev */
    scan_procs();
    for (int i = 0; i < g_nprocs; i++) {
        g_procs[i].total_prev = g_procs[i].total_cur;
    }

    while (1) {
        /* Sleep 1 second */
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
            if (g_winch) {
                g_winch = 0;
                update_winsize();
            }
        }

        /* Second CPU sample */
        sample_cpus(cpu_b, &g_ncpus);

        /* Compute system-wide CPU delta (jiffies) for process % */
        CpuSample *prev = &cpu_a[0];
        CpuSample *cur  = &cpu_b[0];
        unsigned long long sys_prev =
            prev->user + prev->nice + prev->system + prev->idle +
            prev->iowait + prev->irq + prev->softirq + prev->steal;
        unsigned long long sys_cur  =
            cur->user  + cur->nice  + cur->system  + cur->idle  +
            cur->iowait + cur->irq  + cur->softirq + cur->steal;
        unsigned long long sys_delta = sys_cur - sys_prev;

        /* Save previous per-process totals */
        static unsigned long long prev_totals[MAX_PROCS];
        static int prev_pids[MAX_PROCS];
        int prev_n = g_nprocs;

        for (int i = 0; i < prev_n; i++) {
            prev_pids[i]   = g_procs[i].pid;
            prev_totals[i] = g_procs[i].total_cur;
        }

        /* Re-scan processes */
        scan_procs();

        /* Assign previous totals */
        for (int i = 0; i < g_nprocs; i++) {
            g_procs[i].total_prev = 0;
            for (int j = 0; j < prev_n; j++) {
                if (prev_pids[j] == g_procs[i].pid) {
                    g_procs[i].total_prev = prev_totals[j];
                    break;
                }
            }
        }

        compute_proc_cpu(sys_delta);
        scan_compat_layers();

        /* ── Draw UI ── */
        if (g_winch) {
            g_winch = 0;
            update_winsize();
        }

        /* Move cursor to top-left, don't clear (reduces flicker) */
        safe_write(ANSI_HOME, strlen(ANSI_HOME));

        draw_header();
        tui_printf(ANSI_ERASE_LINE "\n");
        draw_cpu_section(cpu_a, cpu_b, g_ncpus);
        tui_printf(ANSI_ERASE_LINE "\n");
        draw_mem_section();
        tui_printf(ANSI_ERASE_LINE "\n");
        draw_compat_section();
        tui_printf(ANSI_ERASE_LINE "\n");
        draw_procs_section();
        tui_printf(ANSI_ERASE_LINE "\n");
        tui_printf("  Press Ctrl-C to quit." ANSI_ERASE_LINE "\n");

        /* Swap CPU samples */
        memcpy(cpu_a, cpu_b, sizeof(cpu_a));
    }

    return 0;
}
