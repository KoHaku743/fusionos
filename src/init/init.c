#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>

/**
 * FusionOS init (PID 1)
 *
 * DOS-first design: after mounting essential filesystems, this init
 * spawns COMMAND.COM (fsh) — the same way classic DOS boots directly
 * into COMMAND.COM after IO.SYS and MSDOS.SYS have finished.
 *
 * Responsibilities:
 *   - Mount proc/sys/dev/tmp
 *   - Set DOS-style environment variables
 *   - Spawn COMMAND.COM in a loop (respawn on exit, like a DOS warm-boot)
 *   - Reap orphaned child processes
 */

/* Signal handler for SIGCHLD - reap children */
static volatile sig_atomic_t child_exited = 0;

static void sigchld_handler(int sig) {
    (void) sig;
    child_exited = 1;
}

/**
 * Mount a filesystem with error reporting
 */
static int mount_fs(const char *source, const char *target, const char *type,
                    unsigned long flags, const void *data) {
    if (mount(source, target, type, flags, data) == -1) {
        perror("mount");
        fprintf(stderr, "mount: %s failed\n", target);
        return -1;
    }
    return 0;
}

/**
 * Initialize the filesystem mounts and DOS-style environment
 */
static int init_system(void) {
    printf("FusionOS init (PID 1) starting...\n");

    struct stat sb;
    /* Ensure /proc is mounted */
    if (stat("/proc/version", &sb) != 0) {
        printf("Mounting /proc...\n");
        mount_fs("proc", "/proc", "proc", 0, NULL);
    }

    /* Ensure /sys is mounted */
    if (stat("/sys/class", &sb) != 0) {
        printf("Mounting /sys...\n");
        mount_fs("sysfs", "/sys", "sysfs", 0, NULL);
    }

    /* Ensure /dev is mounted */
    if (stat("/dev/null", &sb) != 0) {
        printf("Mounting /dev...\n");
        mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    }

    /* Ensure /tmp (C:\TEMP equivalent) is mounted */
    if (stat("/tmp", &sb) != 0) {
        mkdir("/tmp", 0777);
    }
    mount_fs("tmpfs", "/tmp", "tmpfs", 0, "mode=1777");

    printf("System mounts initialized\n");

    /*
     * DOS-style environment: COMMAND.COM++ (fsh) reads these on startup.
     * PATH uses DOS-style semicolon separators (fsh translates at lookup).
     */
    setenv("PATH",    "/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin", 1);
    setenv("HOME",    "/root", 1);
    setenv("TERM",    "linux", 1);
    setenv("COMSPEC", "C:\\COMMAND.COM", 1);
    setenv("OS",      "FusionOS", 1);
    setenv("TEMP",    "C:\\TEMP", 1);
    setenv("TMP",     "C:\\TEMP", 1);

    return 0;
}

/**
 * Spawn a shell process
 */
static pid_t spawn_shell(void) {
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        setsid();
        
        /* Open stdin/stdout/stderr */
        int fd_in = open("/dev/console", O_RDONLY);
        int fd_out = open("/dev/console", O_WRONLY);
        int fd_err = open("/dev/console", O_WRONLY);
        
        if (fd_in == -1) fd_in = open("/dev/null", O_RDONLY);
        if (fd_out == -1) fd_out = open("/dev/null", O_WRONLY);
        if (fd_err == -1) fd_err = open("/dev/null", O_WRONLY);
        
        if (fd_in != STDIN_FILENO) dup2(fd_in, STDIN_FILENO);
        if (fd_out != STDOUT_FILENO) dup2(fd_out, STDOUT_FILENO);
        if (fd_err != STDERR_FILENO) dup2(fd_err, STDERR_FILENO);
        
        close(fd_in);
        close(fd_out);
        close(fd_err);
        
        /* Execute COMMAND.COM (fsh — the FusionOS DOS shell) */
        execl("/bin/fsh", "COMMAND.COM", NULL);
        execl("/usr/bin/fsh", "COMMAND.COM", NULL);

        /* Fallback to BusyBox ash if fsh is not yet installed */
        printf("COMMAND.COM (fsh) not found, trying busybox sh\n");
        execl("/bin/sh", "sh", NULL);
        
        /* Ultimate fallback */
        fprintf(stderr, "Failed to exec shell\n");
        _exit(127);
    }
    
    /* Parent returns child PID */
    return pid;
}

/**
 * Reap all dead children
 */
static void reap_children(void) {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            printf("[%d] exited with status %d\n", pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[%d] killed by signal %d\n", pid, WTERMSIG(status));
        }
    }
}

/**
 * Main init loop
 */
int main(void) {
    if (init_system() != 0) {
        fprintf(stderr, "System initialization failed\n");
        return 1;
    }
    
    /* Setup signal handler for child reaping */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    
    /* Ignore SIGTERM and SIGINT - don't let shells kill init */
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    
    printf("FusionOS init ready. Starting COMMAND.COM...\n");
    printf("=== FusionOS — MS-DOS Evolved ===\n");
    
    /* Main loop: keep spawning shell if it exits */
    while (1) {
        pid_t shell_pid = spawn_shell();
        
        if (shell_pid == -1) {
            sleep(1);
            continue;
        }
        
        /* Wait for shell to exit */
        int status;
        while (waitpid(shell_pid, &status, 0) == -1) {
            if (errno != EINTR) {
                sleep(1);
                break;
            }
        }
        
        /* Reap any other dead children */
        if (child_exited) {
            child_exited = 0;
            reap_children();
        }

        /* Warm-boot: respawn COMMAND.COM after a brief pause */
        sleep(1);
        printf("FusionOS: COMMAND.COM exited, warm-booting...\n");
    }
    
    return 0;
}
