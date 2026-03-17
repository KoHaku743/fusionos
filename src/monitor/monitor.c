#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

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

/**
 * Read a file into a buffer, return bytes read or -1.
 */
static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;

    ssize_t n = 0;
    ssize_t total = 0;
    size_t remaining = size - 1;

    while (remaining > 0) {
        n = read(fd, buf + total, remaining);
        if (n == -1) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += n;
        remaining -= (size_t)n;
    }

    buf[total] = '\0';
    close(fd);
    return (int)total;
}

/**
 * CPU usage: read /proc/stat, extract idle and total jiffies.
 */
typedef struct {
    unsigned long long user;
    unsigned long long nice_time;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
} CpuStat;

static int read_cpu_stat(CpuStat *cpu) {
    char buf[512];
    if (read_file(PROC_STAT, buf, sizeof(buf)) < 0) return -1;

    int parsed = sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu",
                        &cpu->user, &cpu->nice_time, &cpu->system,
                        &cpu->idle, &cpu->iowait, &cpu->irq, &cpu->softirq);
    return (parsed == 7) ? 0 : -1;
}

static unsigned long long cpu_total(const CpuStat *c) {
    return c->user + c->nice_time + c->system + c->idle +
           c->iowait + c->irq + c->softirq;
}

static unsigned long long cpu_idle(const CpuStat *c) {
    return c->idle + c->iowait;
}

/**
 * Print a usage bar for a given percentage (0–100).
 */
static void print_bar(double pct, int width) {
    int filled = (int)(pct / 100.0 * width);
    printf("[");
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            if (pct > 80.0) printf("%s|%s", ANSI_RED, ANSI_RESET);
            else if (pct > 50.0) printf("%s|%s", ANSI_YELLOW, ANSI_RESET);
            else printf("%s|%s", ANSI_GREEN, ANSI_RESET);
        } else {
            printf(" ");
        }
    }
    printf("]");
}

/**
 * Display CPU usage (measured over a short interval).
 */
static void show_cpu(void) {
    CpuStat c1, c2;

    if (read_cpu_stat(&c1) != 0) {
        printf("  CPU: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    usleep(200000); /* 200 ms sample interval */

    if (read_cpu_stat(&c2) != 0) {
        printf("  CPU: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    unsigned long long total_delta = cpu_total(&c2) - cpu_total(&c1);
    unsigned long long idle_delta  = cpu_idle(&c2)  - cpu_idle(&c1);

    double usage = 0.0;
    if (total_delta > 0) {
        usage = 100.0 * (1.0 - (double)idle_delta / (double)total_delta);
    }

    printf("  CPU  %5.1f%%  ", usage);
    print_bar(usage, 30);
    printf("\n");
}

/**
 * Display memory usage from /proc/meminfo.
 */
static void show_memory(void) {
    char buf[2048];
    if (read_file(PROC_MEMINFO, buf, sizeof(buf)) < 0) {
        printf("  MEM: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    unsigned long mem_total = 0, mem_free = 0, mem_buffers = 0,
                  mem_cached = 0, mem_available = 0;
    char line[128];
    const char *p = buf;

    while (*p) {
        /* Read one line */
        size_t i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) {
            line[i++] = *p++;
        }
        if (*p == '\n') p++;
        line[i] = '\0';

        unsigned long val = 0;
        if (sscanf(line, "MemTotal: %lu", &val) == 1) mem_total = val;
        else if (sscanf(line, "MemFree: %lu", &val) == 1) mem_free = val;
        else if (sscanf(line, "Buffers: %lu", &val) == 1) mem_buffers = val;
        else if (sscanf(line, "Cached: %lu", &val) == 1) mem_cached = val;
        else if (sscanf(line, "MemAvailable: %lu", &val) == 1) mem_available = val;
    }

    if (mem_total == 0) {
        printf("  MEM: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    /* Use MemAvailable if available, otherwise estimate */
    unsigned long used;
    if (mem_available > 0) {
        used = mem_total - mem_available;
    } else {
        used = mem_total - mem_free - mem_buffers - mem_cached;
    }

    double pct = (mem_total > 0) ? 100.0 * (double)used / (double)mem_total : 0.0;

    printf("  MEM  %5.1f%%  ", pct);
    print_bar(pct, 30);
    printf("  %lu/%lu MB\n", used / 1024, mem_total / 1024);
}

/**
 * Display system uptime from /proc/uptime.
 */
static void show_uptime(void) {
    char buf[64];
    if (read_file(PROC_UPTIME, buf, sizeof(buf)) < 0) return;

    double up_secs = 0.0;
    sscanf(buf, "%lf", &up_secs);

    unsigned long up = (unsigned long)up_secs;
    unsigned long days  = up / 86400;
    unsigned long hours = (up % 86400) / 3600;
    unsigned long mins  = (up % 3600) / 60;
    unsigned long secs  = up % 60;

    printf("  Uptime: ");
    if (days > 0) printf("%lu day%s, ", days, days == 1 ? "" : "s");
    printf("%02lu:%02lu:%02lu\n", hours, mins, secs);
}

/**
 * Display load averages from /proc/loadavg.
 */
static void show_loadavg(void) {
    char buf[64];
    if (read_file(PROC_LOADAVG, buf, sizeof(buf)) < 0) return;

    double l1, l5, l15;
    int running = 0, total = 0;

    sscanf(buf, "%lf %lf %lf %d/%d", &l1, &l5, &l15, &running, &total);
    printf("  Load avg: %.2f  %.2f  %.2f  (1 min / 5 min / 15 min)\n",
           l1, l5, l15);
    if (total > 0) {
        printf("  Tasks: %d running / %d total\n", running, total);
    }
}

/**
 * Count entries in /proc that look like PIDs (process list).
 */
static int count_processes(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        /* Check if name is all digits */
        int is_pid = 1;
        for (int i = 0; ent->d_name[i]; i++) {
            if (ent->d_name[i] < '0' || ent->d_name[i] > '9') {
                is_pid = 0;
                break;
            }
        }
        if (is_pid && ent->d_name[0] != '\0') count++;
    }

    closedir(d);
    return count;
}

/**
 * Show top processes by reading /proc/<pid>/stat.
 */
static void show_processes(int max_procs) {
    DIR *d = opendir("/proc");
    if (!d) {
        printf("  Processes: %s(unavailable)%s\n", ANSI_RED, ANSI_RESET);
        return;
    }

    printf("  %-8s %-20s %-6s %s\n", "PID", "Name", "State", "VmRSS (kB)");
    printf("  %-8s %-20s %-6s %s\n", "---", "----", "-----", "----------");

    struct dirent *ent;
    int shown = 0;

    while ((ent = readdir(d)) != NULL && shown < max_procs) {
        /* Only process numeric entries (PIDs) */
        int is_pid = 1;
        for (int i = 0; ent->d_name[i]; i++) {
            if (ent->d_name[i] < '0' || ent->d_name[i] > '9') {
                is_pid = 0;
                break;
            }
        }
        if (!is_pid || ent->d_name[0] == '\0') continue;

        /* PID string from /proc is always small, but guard against long names */
        if (strlen(ent->d_name) > 16) continue;

        char stat_path[280];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/status", ent->d_name);

        char buf[512];
        if (read_file(stat_path, buf, sizeof(buf)) < 0) continue;

        char name[64] = "(unknown)";
        char state[8] = "?";
        unsigned long vm_rss = 0;

        const char *p = buf;
        char line[128];

        while (*p) {
            size_t i = 0;
            while (*p && *p != '\n' && i < sizeof(line) - 1) {
                line[i++] = *p++;
            }
            if (*p == '\n') p++;
            line[i] = '\0';

            char tmp[64];
            unsigned long val2 = 0;
            if (sscanf(line, "Name: %63s", tmp) == 1) {
                snprintf(name, sizeof(name), "%s", tmp);
            } else if (sscanf(line, "State: %7s", state) == 1) {
                /* state filled directly with bounded width */
            } else if (sscanf(line, "VmRSS: %lu", &val2) == 1) {
                vm_rss = val2;
            }
        }

        printf("  %-8s %-20s %-6s %lu\n", ent->d_name, name, state, vm_rss);
        shown++;
    }

    closedir(d);

    int total = count_processes();
    if (total > 0) {
        printf("  ... %d total processes\n", total);
    }
}

/**
 * Display kernel version from /proc/version.
 */
static void show_kernel(void) {
    char buf[256];
    if (read_file(PROC_VERSION, buf, sizeof(buf)) < 0) return;

    /* Trim at newline */
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    printf("  Kernel: %s\n", buf);
}

/**
 * Print usage banner.
 */
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -1         One-shot display (default)\n");
    printf("  -c         Continuous mode (refresh every second)\n");
    printf("  -p <n>     Show top N processes (default: 10)\n");
    printf("  -h         Show this help\n");
}

/**
 * Display the full system status report.
 */
static void display_report(int max_procs) {
    printf("%s╔══════════════════════════════════════════════╗%s\n",
           ANSI_CYAN, ANSI_RESET);
    printf("%s║%s  %sFusionOS System Monitor%s                       %s║%s\n",
           ANSI_CYAN, ANSI_RESET, ANSI_BOLD, ANSI_RESET, ANSI_CYAN, ANSI_RESET);
    printf("%s╚══════════════════════════════════════════════╝%s\n",
           ANSI_CYAN, ANSI_RESET);

    printf("\n%s[ System ]%s\n", ANSI_YELLOW, ANSI_RESET);
    show_kernel();
    show_uptime();
    show_loadavg();

    printf("\n%s[ Resources ]%s\n", ANSI_YELLOW, ANSI_RESET);
    show_cpu();
    show_memory();

    printf("\n%s[ Processes (top %d) ]%s\n", ANSI_YELLOW, max_procs, ANSI_RESET);
    show_processes(max_procs);

    printf("\n");
}

int main(int argc, char *argv[]) {
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
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (continuous) {
        /* Clear screen once, then loop */
        printf("\x1b[2J\x1b[H");
        fflush(stdout);

        while (1) {
            printf("\x1b[H"); /* Move cursor to home */
            display_report(max_procs);
            fflush(stdout);
            sleep(1);
        }
    } else {
        display_report(max_procs);
    }

    return 0;
}
