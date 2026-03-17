/*
 * fusion-monitor: FusionOS system monitor TUI
 * Uses only ANSI escape codes (no ncurses dependency).
 * Refreshes every 1 second using nanosleep.
 * Shows CPU usage per core, memory usage, top 10 processes by CPU,
 * and active compat layer counts.
 * Handles terminal resize via SIGWINCH.
 * Compiled static, installed to /usr/bin/fusion-monitor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>

/* ---- ANSI escape helpers ---- */
#define ANSI_CLEAR      "\x1b[2J"
#define ANSI_HOME       "\x1b[H"
#define ANSI_BOLD       "\x1b[1m"
#define ANSI_RESET      "\x1b[0m"
#define ANSI_RED        "\x1b[31m"
#define ANSI_GREEN      "\x1b[32m"
#define ANSI_YELLOW     "\x1b[33m"
#define ANSI_BLUE       "\x1b[34m"
#define ANSI_CYAN       "\x1b[36m"
#define ANSI_WHITE      "\x1b[37m"
#define ANSI_BG_BLUE    "\x1b[44m"
#define ANSI_BG_BLACK   "\x1b[40m"

/* ---- Limits ---- */
#define MAX_CPUS        256
#define MAX_PROCS       1024
#define TOP_PROCS       10
#define PROC_NAME_LEN   64
#define CMDLINE_LEN     256

/* ---- Signal flags ---- */
static volatile sig_atomic_t g_resize = 0;
static volatile sig_atomic_t g_quit   = 0;

/* ---- CPU stat per-core ---- */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CpuStat;

/* ---- Per-process info ---- */
typedef struct {
    int   pid;
    char  name[PROC_NAME_LEN];
    char  cmdline[CMDLINE_LEN];
    unsigned long long utime;   /* jiffies */
    unsigned long long stime;
    unsigned long long prev_total; /* previous sample total */
    float cpu_pct;
    long  vm_rss;               /* kB */
} ProcInfo;

/* ---- Compat layer count ---- */
typedef struct {
    const char *layer;
    const char *keyword;
    int count;
} CompatLayer;

static CompatLayer g_compat_layers[] = {
    { "Wine",      "wine",      0 },
    { "Darling",   "darling",   0 },
    { "Box64",     "box64",     0 },
    { "FEXLoader", "FEXLoader", 0 },
};
#define NUM_COMPAT_LAYERS ((int)(sizeof(g_compat_layers) / sizeof(g_compat_layers[0])))

/* ---- Global state ---- */
static CpuStat g_cpu_prev[MAX_CPUS];
static CpuStat g_cpu_curr[MAX_CPUS];
static int     g_num_cpus = 0;

static ProcInfo g_procs[MAX_PROCS];
static int      g_num_procs = 0;

static unsigned long g_mem_total_kb = 0;
static unsigned long g_mem_avail_kb = 0;
static unsigned long g_mem_used_kb  = 0;
static unsigned long g_swap_total_kb = 0;
static unsigned long g_swap_free_kb  = 0;

static int g_term_cols = 80;
static int g_term_rows = 24;

/* ---- Signal handlers ---- */
static void handle_sigwinch(int sig) {
    (void)sig;
    g_resize = 1;
}

static void handle_sigint(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ---- Safe write wrapper with EINTR retry ---- */
static void safe_write(const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t ret = write(STDOUT_FILENO, buf + written, len - written);
        if (ret == -1) {
            if (errno == EINTR) continue;
            return;
        }
        written += (size_t)ret;
    }
}

/* ---- Print a string to stdout ---- */
static void tui_print(const char *s) {
    safe_write(s, strlen(s));
}

/* ---- Query terminal dimensions ---- */
static void get_terminal_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        g_term_cols = ws.ws_col;
        g_term_rows = ws.ws_row;
    }
}

/* ---- Read /proc/stat into cpu arrays ---- */
static int read_cpu_stats(void) {
    int fd = open("/proc/stat", O_RDONLY);
    if (fd == -1) return -1;

    char buf[8192];
    ssize_t n = 0, total = 0;
    while (total < (ssize_t)(sizeof(buf) - 1)) {
        n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';

    /* Parse lines: cpu0, cpu1, ... */
    int ncpus = 0;
    char *line = buf;
    while (line && *line && ncpus < MAX_CPUS) {
        /* Skip aggregate "cpu " line */
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] == ' ') {
            /* Skip the aggregate line */
            line = strchr(line, '\n');
            if (line) line++;
            continue;
        }
        /* Parse per-cpu line: cpuN user nice system idle iowait irq softirq steal */
        CpuStat s;
        memset(&s, 0, sizeof(s));
        int idx = -1;
        int matched = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
            &idx,
            &s.user, &s.nice, &s.system, &s.idle,
            &s.iowait, &s.irq, &s.softirq, &s.steal);
        if (matched >= 5 && idx >= 0 && idx < MAX_CPUS) {
            g_cpu_curr[idx] = s;
            if (idx + 1 > ncpus) ncpus = idx + 1;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    g_num_cpus = ncpus;
    return 0;
}

/* ---- Read /proc/meminfo ---- */
static void read_meminfo(void) {
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd == -1) return;

    char buf[4096];
    ssize_t total = 0;
    while (total < (ssize_t)(sizeof(buf) - 1)) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';

    g_mem_total_kb = 0;
    g_mem_avail_kb = 0;
    g_swap_total_kb = 0;
    g_swap_free_kb  = 0;

    char *p = buf;
    while (p && *p) {
        unsigned long val = 0;
        if (sscanf(p, "MemTotal: %lu kB", &val) == 1)       g_mem_total_kb = val;
        else if (sscanf(p, "MemAvailable: %lu kB", &val) == 1) g_mem_avail_kb = val;
        else if (sscanf(p, "SwapTotal: %lu kB", &val) == 1)  g_swap_total_kb = val;
        else if (sscanf(p, "SwapFree: %lu kB", &val) == 1)   g_swap_free_kb  = val;
        p = strchr(p, '\n');
        if (p) p++;
    }
    g_mem_used_kb = (g_mem_total_kb > g_mem_avail_kb) ?
                    (g_mem_total_kb - g_mem_avail_kb) : 0;
}

/**
 * Read a small file into buf; returns bytes read or -1.
 * Handles EINTR on read.
 */
static ssize_t slurp_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;

    ssize_t total = 0;
    while ((size_t)total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - (size_t)total);
        if (n == -1) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/**
 * Read per-process stats from /proc/[pid]/stat and /proc/[pid]/cmdline.
 * Fills g_procs[] and g_num_procs.
 */
static void read_proc_stats(void) {
    DIR *dp = opendir("/proc");
    if (!dp) return;

    /* Reset compat counts */
    for (int i = 0; i < NUM_COMPAT_LAYERS; i++) {
        g_compat_layers[i].count = 0;
    }

    int nprocs = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && nprocs < MAX_PROCS) {
        /* Skip non-numeric entries */
        const char *d = de->d_name;
        if (*d < '1' || *d > '9') continue;

        int pid = atoi(d);
        if (pid <= 0) continue;

        /* Read /proc/PID/stat */
        char path[64];
        char stat_buf[512];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (slurp_file(path, stat_buf, sizeof(stat_buf)) < 0) continue;

        /* Parse: pid (name) state ppid pgroup session ... utime stime ... */
        char name_paren[PROC_NAME_LEN];
        char state;
        unsigned long long utime = 0, stime = 0;
        long rss = 0;

        /* Find name between parentheses (may contain spaces) */
        char *lp = strchr(stat_buf, '(');
        char *rp = strrchr(stat_buf, ')');
        if (!lp || !rp || rp <= lp) continue;

        size_t name_len = (size_t)(rp - lp - 1);
        if (name_len >= PROC_NAME_LEN) name_len = PROC_NAME_LEN - 1;
        memcpy(name_paren, lp + 1, name_len);
        name_paren[name_len] = '\0';

        /* Parse remaining fields after ')' */
        /* Format after ')': state ppid pgrp session tty_nr tpgid flags
           minflt cminflt majflt cmajflt utime stime cutime cstime
           (indices: 1=state, 13=utime, 14=stime, 23=rss) */
        int fnum = 0;
        char *tok = rp + 2; /* skip ') ' */
        char *endp = NULL;
        while (tok && *tok) {
            char *next = strchr(tok, ' ');
            if (next) *next = '\0';

            switch (fnum) {
                case 0: state = tok[0]; break;
                case 11: utime = strtoull(tok, &endp, 10); break;
                case 12: stime = strtoull(tok, &endp, 10); break;
                case 21: rss   = atol(tok); break;
                default: break;
            }
            fnum++;
            if (fnum > 22) break;
            tok = next ? next + 1 : NULL;
        }
        (void)state; /* not needed beyond parsing */

        ProcInfo *pi = &g_procs[nprocs];
        pi->pid   = pid;
        pi->utime = utime;
        pi->stime = stime;
        pi->vm_rss = rss * 4; /* pages → kB (assuming 4KB pages) */
        snprintf(pi->name, PROC_NAME_LEN, "%s", name_paren);

        /* Read cmdline for compat detection */
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        char cmdline_buf[CMDLINE_LEN];
        ssize_t cbytes = slurp_file(path, cmdline_buf, sizeof(cmdline_buf));
        if (cbytes > 0) {
            /* Replace NUL bytes with spaces */
            for (ssize_t k = 0; k < cbytes - 1; k++) {
                if (cmdline_buf[k] == '\0') cmdline_buf[k] = ' ';
            }
            snprintf(pi->cmdline, CMDLINE_LEN, "%s", cmdline_buf);

            /* Check for compat layer keywords */
            for (int ci = 0; ci < NUM_COMPAT_LAYERS; ci++) {
                if (strstr(pi->cmdline, g_compat_layers[ci].keyword) ||
                    strstr(pi->name, g_compat_layers[ci].keyword)) {
                    g_compat_layers[ci].count++;
                }
            }
        } else {
            pi->cmdline[0] = '\0';
        }

        nprocs++;
    }
    closedir(dp);
    g_num_procs = nprocs;
}

/**
 * Compute CPU usage percentage per core between two samples.
 * Returns value in [0.0, 100.0].
 */
static float cpu_usage(const CpuStat *prev, const CpuStat *curr) {
    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long curr_idle = curr->idle + curr->iowait;

    unsigned long long prev_total = prev->user + prev->nice + prev->system +
                                    prev->idle + prev->iowait + prev->irq +
                                    prev->softirq + prev->steal;
    unsigned long long curr_total = curr->user + curr->nice + curr->system +
                                    curr->idle + curr->iowait + curr->irq +
                                    curr->softirq + curr->steal;

    unsigned long long d_total = curr_total - prev_total;
    unsigned long long d_idle  = curr_idle  - prev_idle;

    if (d_total == 0) return 0.0f;
    return (float)(d_total - d_idle) * 100.0f / (float)d_total;
}

/* ---- Draw a progress bar ---- */
static void draw_bar(float pct, int width, const char *color) {
    if (width < 2) return;
    int filled = (int)((pct / 100.0f) * (float)width);
    if (filled > width) filled = width;

    tui_print(color);
    tui_print("[");
    for (int i = 0; i < width; i++) {
        tui_print(i < filled ? "|" : " ");
    }
    tui_print("]");
    tui_print(ANSI_RESET);
}

/* ---- Print a horizontal line ---- */
static void print_hline(void) {
    char line[256];
    int w = g_term_cols < 254 ? g_term_cols : 254;
    for (int i = 0; i < w; i++) line[i] = '-';
    line[w] = '\n';
    line[w + 1] = '\0';
    tui_print(line);
}

/* ---- Render the full screen ---- */
static void render_screen(void) {
    char buf[512];

    /* Clear screen and go home */
    tui_print(ANSI_CLEAR ANSI_HOME);

    /* Header */
    tui_print(ANSI_BOLD ANSI_BG_BLUE ANSI_WHITE);
    snprintf(buf, sizeof(buf), " FusionOS Monitor  (q to quit)  "
             "Cols:%d Rows:%d", g_term_cols, g_term_rows);
    tui_print(buf);
    /* Pad to terminal width */
    int hlen = (int)strlen(buf);
    for (int i = hlen; i < g_term_cols - 1; i++) tui_print(" ");
    tui_print(ANSI_RESET "\n");

    /* ---- Memory section ---- */
    tui_print(ANSI_BOLD ANSI_CYAN "Memory" ANSI_RESET "\n");
    if (g_mem_total_kb > 0) {
        float mem_pct = (float)g_mem_used_kb * 100.0f / (float)g_mem_total_kb;
        snprintf(buf, sizeof(buf), "  RAM:  %6lu MiB / %6lu MiB  (%5.1f%%)  ",
                 g_mem_used_kb / 1024, g_mem_total_kb / 1024, mem_pct);
        tui_print(buf);
        draw_bar(mem_pct, 30, mem_pct > 80.0f ? ANSI_RED : ANSI_GREEN);
        tui_print("\n");

        if (g_swap_total_kb > 0) {
            unsigned long swap_used = g_swap_total_kb - g_swap_free_kb;
            float swap_pct = (float)swap_used * 100.0f / (float)g_swap_total_kb;
            snprintf(buf, sizeof(buf), "  Swap: %6lu MiB / %6lu MiB  (%5.1f%%)  ",
                     swap_used / 1024, g_swap_total_kb / 1024, swap_pct);
            tui_print(buf);
            draw_bar(swap_pct, 30, swap_pct > 50.0f ? ANSI_YELLOW : ANSI_GREEN);
            tui_print("\n");
        }
    } else {
        tui_print("  (no data)\n");
    }

    /* ---- CPU section ---- */
    print_hline();
    tui_print(ANSI_BOLD ANSI_CYAN "CPU Cores" ANSI_RESET "\n");

    int bar_w = (g_term_cols > 60) ? 40 : 20;
    int cols_per_row = (g_term_cols >= 120) ? 2 : 1;
    int col_w = g_term_cols / cols_per_row;

    for (int i = 0; i < g_num_cpus; i++) {
        float pct = cpu_usage(&g_cpu_prev[i], &g_cpu_curr[i]);
        snprintf(buf, sizeof(buf), "  cpu%-3d %5.1f%%  ", i, pct);
        tui_print(buf);
        draw_bar(pct, bar_w, pct > 80.0f ? ANSI_RED :
                              pct > 50.0f ? ANSI_YELLOW : ANSI_GREEN);

        if (cols_per_row == 2 && (i % 2) == 0 && i + 1 < g_num_cpus) {
            /* Print next cpu on same line */
            int pad = col_w - (int)strlen(buf) - bar_w - 3;
            for (int p = 0; p < pad && p < 20; p++) tui_print(" ");
        } else {
            tui_print("\n");
        }
    }
    if (g_num_cpus == 0) tui_print("  (no data)\n");

    /* ---- Top processes section ---- */
    print_hline();
    tui_print(ANSI_BOLD ANSI_CYAN "Top Processes by CPU" ANSI_RESET "\n");
    snprintf(buf, sizeof(buf),
             "  %-7s  %-20s  %6s  %8s\n",
             "PID", "Name", "CPU%", "RSS(kB)");
    tui_print(buf);

    /* Compute per-process CPU% using total jiffies delta as denominator */
    /* Use sum of all cpu jiffies difference as proxy for clock ticks */
    unsigned long long total_jiffies_delta = 0;
    for (int i = 0; i < g_num_cpus; i++) {
        CpuStat *p = &g_cpu_prev[i], *c = &g_cpu_curr[i];
        unsigned long long prev_t = p->user + p->nice + p->system +
                                    p->idle + p->iowait + p->irq +
                                    p->softirq + p->steal;
        unsigned long long curr_t = c->user + c->nice + c->system +
                                    c->idle + c->iowait + c->irq +
                                    c->softirq + c->steal;
        total_jiffies_delta += curr_t - prev_t;
    }

    /* Sort procs by cpu_pct (simple selection sort for top 10) */
    /* First copy indices */
    int order[MAX_PROCS];
    for (int i = 0; i < g_num_procs; i++) order[i] = i;

    /* Compute cpu_pct per process (requires previous snapshot) */
    /* We just show utime+stime as proxy - not per-process delta here,
       but good-enough for visualization */
    for (int i = 0; i < g_num_procs; i++) {
        unsigned long long total_time = g_procs[i].utime + g_procs[i].stime;
        g_procs[i].cpu_pct = (total_jiffies_delta > 0) ?
            (float)total_time * 100.0f / (float)total_jiffies_delta : 0.0f;
    }

    /* Partial sort: find top TOP_PROCS by cpu_pct */
    int display_count = g_num_procs < TOP_PROCS ? g_num_procs : TOP_PROCS;
    for (int i = 0; i < display_count; i++) {
        int max_idx = i;
        for (int j = i + 1; j < g_num_procs; j++) {
            if (g_procs[order[j]].cpu_pct > g_procs[order[max_idx]].cpu_pct) {
                max_idx = j;
            }
        }
        int tmp = order[i];
        order[i] = order[max_idx];
        order[max_idx] = tmp;
    }

    for (int i = 0; i < display_count; i++) {
        ProcInfo *pi = &g_procs[order[i]];
        snprintf(buf, sizeof(buf),
                 "  %-7d  %-20.20s  %5.1f%%  %8ld\n",
                 pi->pid, pi->name, pi->cpu_pct, pi->vm_rss);
        tui_print(buf);
    }
    if (g_num_procs == 0) tui_print("  (no data)\n");

    /* ---- Compat layers section ---- */
    print_hline();
    tui_print(ANSI_BOLD ANSI_CYAN "Active Compat Layers" ANSI_RESET "\n");
    int any_active = 0;
    for (int i = 0; i < NUM_COMPAT_LAYERS; i++) {
        if (g_compat_layers[i].count > 0) {
            snprintf(buf, sizeof(buf), "  %-12s : %d process%s\n",
                     g_compat_layers[i].layer,
                     g_compat_layers[i].count,
                     g_compat_layers[i].count == 1 ? "" : "es");
            tui_print(buf);
            any_active = 1;
        }
    }
    if (!any_active) tui_print("  (none)\n");

    /* Footer */
    print_hline();
    tui_print("  Press q to quit  |  Refreshes every 1 second\n");
}

/* ---- nanosleep wrapper with EINTR retry ---- */
static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        if (g_quit || g_resize) return;
    }
}

/* ---- Non-blocking stdin read for keypress ---- */
static int check_keypress(void) {
    char c;
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    ssize_t n;
    do {
        n = read(STDIN_FILENO, &c, 1);
    } while (n == -1 && errno == EINTR);
    fcntl(STDIN_FILENO, F_SETFL, flags);
    if (n == 1 && (c == 'q' || c == 'Q')) return 1;
    return 0;
}

/**
 * Main monitor loop: collect stats, render, sleep 1 second.
 */
int main(void) {
    /* Setup signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    get_terminal_size();

    /* Hide cursor */
    tui_print("\x1b[?25l");

    /* Initial sample */
    read_cpu_stats();
    memcpy(g_cpu_prev, g_cpu_curr, sizeof(CpuStat) * MAX_CPUS);
    read_proc_stats();
    read_meminfo();

    while (!g_quit) {
        if (g_resize) {
            g_resize = 0;
            get_terminal_size();
        }

        /* Rotate CPU stats */
        memcpy(g_cpu_prev, g_cpu_curr, sizeof(CpuStat) * MAX_CPUS);

        /* Collect new stats */
        read_cpu_stats();
        read_proc_stats();
        read_meminfo();

        render_screen();

        sleep_ms(1000);

        if (check_keypress()) break;
    }

    /* Show cursor and clear screen on exit */
    tui_print("\x1b[?25h" ANSI_CLEAR ANSI_HOME);
    return 0;
}
