#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * fusion-monitor — FusionOS TUI system monitor
 *
 * Uses ANSI escape codes only (no ncurses).
 * Refreshes every 1 second using nanosleep().
 * Handles terminal resize via SIGWINCH.
 * Shows: per-core CPU, memory, top processes, active compat layers.
 * -------------------------------------------------------------------- */

#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_RESET   "\x1b[0m"

#define PROC_STAT    "/proc/stat"
#define PROC_MEMINFO "/proc/meminfo"
#define PROC_UPTIME  "/proc/uptime"
#define PROC_LOADAVG "/proc/loadavg"
#define PROC_VERSION "/proc/version"

#define MAX_CORES    128
#define MAX_PROCS    512   /* max entries scanned */
/* Linux PID_MAX_LIMIT is 4194304 (7 decimal digits) */
#define MAX_PID_DIGITS 7

/* SIGWINCH flag — set from signal handler */
static volatile sig_atomic_t g_resize = 0;

/* Terminal dimensions */
static int g_term_cols = 80;
static int g_term_rows = 24;

/* SIGWINCH handler: flag a resize, re-read terminal dimensions */
static void handle_sigwinch(int sig)
{
    (void)sig;
    g_resize = 1;
}

/**
 * Query current terminal dimensions via TIOCGWINSZ.
 */
static void update_term_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        g_term_cols = (int)ws.ws_col;
        g_term_rows = (int)ws.ws_row;
    }
}

/* ------------------------------------------------------------------
 * File reading helper — retries on EINTR
 * ------------------------------------------------------------------ */
/**
 * Read up to size-1 bytes from path into buf (NUL-terminated).
 * Returns bytes read or -1 on failure.
 */
static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;

    ssize_t total = 0;
    size_t  remaining = size - 1;

    while (remaining > 0) {
        ssize_t n = read(fd, buf + total, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total     += n;
        remaining -= (size_t)n;
    }

    buf[total] = '\0';
    close(fd);
    return (int)total;
}

/* ------------------------------------------------------------------
 * CPU per-core statistics
 * ------------------------------------------------------------------ */
typedef struct {
    char               name[16];    /* "cpu", "cpu0", "cpu1", ... */
    unsigned long long user;
    unsigned long long nice_time;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
} CpuStat;

static unsigned long long cpu_total(const CpuStat *c)
{
    return c->user + c->nice_time + c->system + c->idle +
           c->iowait + c->irq + c->softirq;
}

static unsigned long long cpu_idle(const CpuStat *c)
{
    return c->idle + c->iowait;
}

/**
 * Parse /proc/stat and fill cpu_arr[0..max_cores-1].
 * cpu_arr[0] is always the aggregate "cpu" line.
 * Returns the number of entries filled (1 + number of per-core lines).
 */
static int read_all_cpu_stats(CpuStat *cpu_arr, int max_cores)
{
    char buf[8192];
    if (read_file(PROC_STAT, buf, sizeof(buf)) < 0) return 0;

    int count = 0;
    const char *p = buf;

    while (*p && count < max_cores) {
        /* Each line starts with "cpu..." */
        if (strncmp(p, "cpu", 3) != 0) {
            /* Skip non-cpu lines */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        CpuStat *c = &cpu_arr[count];
        int parsed = sscanf(p, "%15s %llu %llu %llu %llu %llu %llu %llu",
                            c->name,
                            &c->user, &c->nice_time, &c->system,
                            &c->idle, &c->iowait, &c->irq, &c->softirq);

        if (parsed >= 5) count++;

        /* Advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    return count;
}

/* ------------------------------------------------------------------
 * Progress bar rendering
 * ------------------------------------------------------------------ */
/**
 * Print a usage bar of the given width for a percentage value (0–100).
 */
static void print_bar(double pct, int width)
{
    if (width < 2) width = 2;
    int filled = (int)(pct / 100.0 * (double)width);
    if (filled > width) filled = width;

    printf("[");
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            if (pct > 80.0)
                printf("%s|%s", ANSI_RED,    ANSI_RESET);
            else if (pct > 50.0)
                printf("%s|%s", ANSI_YELLOW, ANSI_RESET);
            else
                printf("%s|%s", ANSI_GREEN,  ANSI_RESET);
        } else {
            printf(" ");
        }
    }
    printf("]");
}

/* ------------------------------------------------------------------
 * CPU display
 * ------------------------------------------------------------------ */
/**
 * Sample /proc/stat twice with a short gap and display per-core CPU usage.
 */
static void show_cpu(void)
{
    CpuStat s1[MAX_CORES], s2[MAX_CORES];

    int n1 = read_all_cpu_stats(s1, MAX_CORES);
    if (n1 == 0) {
        printf("  CPU: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    /* 100 ms sample interval */
    struct timespec ts = { 0, 100000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { /* retry */ }

    int n2 = read_all_cpu_stats(s2, MAX_CORES);
    if (n2 == 0) {
        printf("  CPU: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    int bar_width = (g_term_cols > 60) ? 30 : 20;

    for (int i = 0; i < n1 && i < n2; i++) {
        unsigned long long tdelta = cpu_total(&s2[i]) - cpu_total(&s1[i]);
        unsigned long long idelta = cpu_idle(&s2[i])  - cpu_idle(&s1[i]);

        double pct = 0.0;
        if (tdelta > 0)
            pct = 100.0 * (1.0 - (double)idelta / (double)tdelta);

        /* Aggregate line: "cpu " = overall, "cpu0".."cpuN" = per-core */
        if (strcmp(s1[i].name, "cpu") == 0) {
            printf("  %sCPU%s  %5.1f%%  ", ANSI_BOLD, ANSI_RESET, pct);
        } else {
            printf("  %-5s %5.1f%%  ", s1[i].name, pct);
        }
        print_bar(pct, bar_width);
        printf("\n");
    }
}

/* ------------------------------------------------------------------
 * Memory display
 * ------------------------------------------------------------------ */
/**
 * Parse /proc/meminfo and display a usage bar.
 */
static void show_memory(void)
{
    char buf[4096];
    if (read_file(PROC_MEMINFO, buf, sizeof(buf)) < 0) {
        printf("  MEM: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    unsigned long mem_total = 0, mem_free = 0, mem_buffers = 0,
                  mem_cached = 0, mem_available = 0;

    const char *p = buf;
    char line[128];

    while (*p) {
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = '\0';

        unsigned long val = 0;
        if      (sscanf(line, "MemTotal: %lu",     &val) == 1) mem_total     = val;
        else if (sscanf(line, "MemFree: %lu",      &val) == 1) mem_free      = val;
        else if (sscanf(line, "Buffers: %lu",      &val) == 1) mem_buffers   = val;
        else if (sscanf(line, "Cached: %lu",       &val) == 1) mem_cached    = val;
        else if (sscanf(line, "MemAvailable: %lu", &val) == 1) mem_available = val;
    }

    if (mem_total == 0) {
        printf("  MEM: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    unsigned long used = (mem_available > 0)
        ? mem_total - mem_available
        : mem_total - mem_free - mem_buffers - mem_cached;

    double pct = (double)used / (double)mem_total * 100.0;
    int bar_width = (g_term_cols > 60) ? 30 : 20;

    printf("  %sMEM%s  %5.1f%%  ", ANSI_BOLD, ANSI_RESET, pct);
    print_bar(pct, bar_width);
    printf("  %lu/%lu MB\n", used / 1024, mem_total / 1024);
}

/* ------------------------------------------------------------------
 * Uptime and load average
 * ------------------------------------------------------------------ */
static void show_uptime(void)
{
    char buf[64];
    if (read_file(PROC_UPTIME, buf, sizeof(buf)) < 0) return;

    double up_secs = 0.0;
    sscanf(buf, "%lf", &up_secs);

    unsigned long up    = (unsigned long)up_secs;
    unsigned long days  = up / 86400UL;
    unsigned long hours = (up % 86400UL) / 3600UL;
    unsigned long mins  = (up % 3600UL)  / 60UL;
    unsigned long secs  = up % 60UL;

    printf("  Uptime: ");
    if (days > 0) printf("%lu day%s, ", days, days == 1 ? "" : "s");
    printf("%02lu:%02lu:%02lu\n", hours, mins, secs);
}

static void show_loadavg(void)
{
    char buf[64];
    if (read_file(PROC_LOADAVG, buf, sizeof(buf)) < 0) return;

    double l1, l5, l15;
    int    running = 0, total = 0;

    sscanf(buf, "%lf %lf %lf %d/%d", &l1, &l5, &l15, &running, &total);
    printf("  Load:   %.2f  %.2f  %.2f  (1m / 5m / 15m)\n", l1, l5, l15);
}

static void show_kernel(void)
{
    char buf[256];
    if (read_file(PROC_VERSION, buf, sizeof(buf)) < 0) return;
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    /* Trim to terminal width */
    int maxw = g_term_cols - 12;
    if (maxw < 10) maxw = 10;
    printf("  Kernel: %.*s\n", maxw, buf);
}

/* ------------------------------------------------------------------
 * Process list (top N by VmRSS as proxy for activity)
 * ------------------------------------------------------------------ */
typedef struct {
    int          pid;
    char         name[64];
    char         state[4];
    unsigned long vm_rss;    /* kB */
    unsigned long long utime; /* from /proc/pid/stat */
} ProcInfo;

/**
 * Comparison function for qsort: descending by utime (CPU usage proxy).
 */
static int proc_cmp(const void *a, const void *b)
{
    const ProcInfo *pa = (const ProcInfo *)a;
    const ProcInfo *pb = (const ProcInfo *)b;
    if (pb->utime > pa->utime) return  1;
    if (pb->utime < pa->utime) return -1;
    return 0;
}

/**
 * Collect info for a single PID from /proc/<pid>/status and /proc/<pid>/stat.
 * Returns 0 on success.
 */
static int read_proc_info(const char *pid_str, ProcInfo *out)
{
    char path[64];
    char buf[2048];

    /* Basic info from /proc/pid/status */
    snprintf(path, sizeof(path), "/proc/%s/status", pid_str);
    if (read_file(path, buf, sizeof(buf)) < 0) return -1;

    out->pid    = atoi(pid_str);
    out->vm_rss = 0;
    snprintf(out->name,  sizeof(out->name),  "(unknown)");
    snprintf(out->state, sizeof(out->state), "?");

    const char *p = buf;
    char line[128];

    while (*p) {
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = '\0';

        char tmp[64];
        unsigned long val = 0;
        if (sscanf(line, "Name: %63s", tmp) == 1)
            snprintf(out->name, sizeof(out->name), "%s", tmp);
        else if (sscanf(line, "State: %3s", out->state) == 1) { /* ok */ }
        else if (sscanf(line, "VmRSS: %lu", &val) == 1)
            out->vm_rss = val;
    }

    /* CPU time from /proc/pid/stat: field 14 = utime */
    snprintf(path, sizeof(path), "/proc/%s/stat", pid_str);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        unsigned long long utime = 0;
        /* Skip past comm field (may contain spaces inside parentheses) */
        const char *comm_end = strrchr(buf, ')');
        if (comm_end) {
            comm_end += 2; /* skip ') ' */
            /* Fields: state(3) ppid(4) pgrp(5) session(6) tty_nr(7)
             *         tpgid(8) flags(9) minflt(10) cminflt(11)
             *         majflt(12) cmajflt(13) utime(14) */
            unsigned long dummy_ul = 0;
            int dummy_i = 0;
            char dummy_c = 0;
            int ret = sscanf(comm_end,
                "%c %d %d %d %d %d %lu %lu %lu %lu %llu",
                &dummy_c, &dummy_i, &dummy_i, &dummy_i,
                &dummy_i, &dummy_i,
                &dummy_ul, &dummy_ul, &dummy_ul, &dummy_ul,
                &utime);
            if (ret >= 11) out->utime = utime;
        }
    }

    return 0;
}

/**
 * Show top N processes by CPU time (from /proc/<pid>/stat utime).
 */
static void show_processes(int max_procs)
{
    DIR *d = opendir("/proc");
    if (!d) {
        printf("  Processes: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    static ProcInfo procs[MAX_PROCS];
    int count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < MAX_PROCS) {
        /* Only numeric entries (PIDs) */
        const char *cp = ent->d_name;
        if (*cp < '1' || *cp > '9') continue;
        int all_digits = 1;
        for (int i = 1; cp[i]; i++) {
            if (cp[i] < '0' || cp[i] > '9') { all_digits = 0; break; }
        }
        if (!all_digits) continue;

        if (read_proc_info(cp, &procs[count]) == 0) count++;
    }
    closedir(d);

    /* Sort by CPU time descending */
    qsort(procs, (size_t)count, sizeof(ProcInfo), proc_cmp);

    int shown = (max_procs < count) ? max_procs : count;

    printf("  " ANSI_BOLD "%-7s %-20s %-5s %-10s %s\n" ANSI_RESET,
           "PID", "Name", "State", "RSS(kB)", "CPU(t)");

    for (int i = 0; i < shown; i++) {
        printf("  %-7d %-20s %-5s %-10lu %llu\n",
               procs[i].pid,
               procs[i].name,
               procs[i].state,
               procs[i].vm_rss,
               procs[i].utime);
    }
    printf("  (%d total processes)\n", count);
}

/* ------------------------------------------------------------------
 * Active compat layer detection
 * ------------------------------------------------------------------ */
typedef struct {
    const char *token;    /* string to look for in /proc/pid/cmdline */
    const char *label;    /* friendly name */
    int         count;    /* number of processes found */
} CompatLayer;

/**
 * Scan /proc/[pid]/cmdline for known compat layer executables.
 * Updates the count field of each layer in the array.
 */
static void scan_compat_layers(CompatLayer *layers, int nlayers)
{
    /* Zero counts */
    for (int i = 0; i < nlayers; i++) layers[i].count = 0;

    DIR *d = opendir("/proc");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *cp = ent->d_name;
        if (*cp < '1' || *cp > '9') continue;
        int all_digits = 1;
        for (int j = 1; cp[j]; j++) {
            if (cp[j] < '0' || cp[j] > '9') { all_digits = 0; break; }
        }
        if (!all_digits) continue;
        /* PIDs are at most MAX_PID_DIGITS digits; skip anything longer */
        if (strlen(cp) > MAX_PID_DIGITS) continue;

        char path[40];
        snprintf(path, sizeof(path), "/proc/%.*s/cmdline", MAX_PID_DIGITS, cp);

        char buf[512];
        int n = read_file(path, buf, sizeof(buf));
        if (n <= 0) continue;

        /* cmdline uses NUL as separator; scan only argv[0] for the token */
        for (int i = 0; i < nlayers; i++) {
            if (strstr(buf, layers[i].token)) {
                layers[i].count++;
                break;
            }
        }
    }
    closedir(d);
}

/**
 * Display active compat layer summary.
 */
static void show_compat_layers(void)
{
    CompatLayer layers[] = {
        { "wine",      "Wine (Windows)",       0 },
        { "darling",   "Darling (macOS)",      0 },
        { "box64",     "Box64 (ARM→x64)",      0 },
        { "FEXLoader", "FEX (ARM64→x64/x86)",  0 },
    };
    int nlayers = (int)(sizeof(layers) / sizeof(layers[0]));

    scan_compat_layers(layers, nlayers);

    int active = 0;
    for (int i = 0; i < nlayers; i++) {
        if (layers[i].count > 0) {
            printf("  " ANSI_GREEN "●" ANSI_RESET " %-28s  %d process%s\n",
                   layers[i].label,
                   layers[i].count,
                   layers[i].count == 1 ? "" : "es");
            active++;
        }
    }
    if (active == 0) {
        printf("  " ANSI_YELLOW "(no active compat layers)" ANSI_RESET "\n");
    }
}

/* ------------------------------------------------------------------
 * Full display refresh
 * ------------------------------------------------------------------ */
/**
 * Render the complete status page to stdout.
 */
static void display_report(int max_procs)
{
    int bar_fill = g_term_cols - 4;
    if (bar_fill < 20) bar_fill = 20;
    if (bar_fill > 70) bar_fill = 70;

    printf(ANSI_CYAN ANSI_BOLD
           "╔══════════════════════════════════════════════╗\n"
           "║  FusionOS System Monitor                     ║\n"
           "╚══════════════════════════════════════════════╝"
           ANSI_RESET "\n");

    printf("\n" ANSI_YELLOW ANSI_BOLD "[ System ]" ANSI_RESET "\n");
    show_kernel();
    show_uptime();
    show_loadavg();

    printf("\n" ANSI_YELLOW ANSI_BOLD "[ CPU & Memory ]" ANSI_RESET "\n");
    show_cpu();
    show_memory();

    printf("\n" ANSI_YELLOW ANSI_BOLD "[ Top %d Processes ]" ANSI_RESET "\n",
           max_procs);
    show_processes(max_procs);

    printf("\n" ANSI_YELLOW ANSI_BOLD "[ Active Compat Layers ]" ANSI_RESET "\n");
    show_compat_layers();

    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * Usage / help
 * ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -1         One-shot display (default)\n");
    printf("  -c         Continuous mode (refresh every second)\n");
    printf("  -p <n>     Show top N processes (default: 10, max: 100)\n");
    printf("  -h         Show this help\n");
}

/* ------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int continuous = 0;
    int max_procs  = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            continuous = 1;
        } else if (strcmp(argv[i], "-1") == 0) {
            continuous = 0;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            i++;
            max_procs = atoi(argv[i]);
            if (max_procs <= 0 || max_procs > 100) max_procs = 10;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Get initial terminal size */
    update_term_size();

    if (continuous) {
        /* Install SIGWINCH handler */
        struct sigaction sa;
        sa.sa_handler = handle_sigwinch;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, NULL);

        /* Clear screen once, then loop */
        printf("\x1b[2J\x1b[H");
        fflush(stdout);

        struct timespec sleep_ts = { 1, 0 };

        while (1) {
            if (g_resize) {
                g_resize = 0;
                update_term_size();
            }
            printf("\x1b[H"); /* Move cursor to top-left */
            display_report(max_procs);

            /* Interruptible sleep: retry on EINTR */
            struct timespec rem = sleep_ts;
            while (nanosleep(&rem, &rem) == -1 && errno == EINTR) {
                if (g_resize) break; /* wake on resize */
            }
        }
    } else {
        display_report(max_procs);
    }

    return 0;
}
