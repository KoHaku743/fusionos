#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Minimal write to stderr for error messages (no printf) */
static void write_str(const char *msg) {
    size_t len = 0;
    while (msg[len]) len++;
    
    ssize_t ret;
    size_t written = 0;
    while (written < len) {
        ret = write(STDERR_FILENO, msg + written, len - written);
        if (ret == -1) {
            if (errno == EINTR) continue;
            break;
        }
        written += ret;
    }
}

/**
 * Mount a filesystem with error handling
 */
static int mount_fs(const char *source, const char *target, const char *type,
                    unsigned long flags, const void *data) {
    if (mount(source, target, type, flags, data) == -1) {
        write_str("mount failed: ");
        write_str(target);
        write_str("\n");
        return -1;
    }
    return 0;
}

/**
 * Run mdev to populate /dev
 */
static int run_mdev(void) {
    pid_t pid = fork();
    if (pid == -1) {
        write_str("fork failed\n");
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        execl("/bin/mdev", "mdev", "-s", NULL);
        /* If exec fails */
        write_str("mdev: exec failed\n");
        _exit(127);
    }
    
    /* Parent: wait for child */
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            write_str("waitpid failed\n");
            return -1;
        }
    }
    
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/**
 * Check if a block device exists and try to mount it
 */
static int try_mount_rootfs(const char *dev_path) {
    struct stat st;
    if (stat(dev_path, &st) != 0) {
        return -1;
    }
    
    if (!S_ISBLK(st.st_mode)) {
        return -1;
    }
    
    /* Try to mount as read-write first, fall back to read-only */
    if (mount(dev_path, "/newroot", "auto", 0, NULL) == 0) {
        return 0;
    }
    
    if (mount(dev_path, "/newroot", "auto", MS_RDONLY, NULL) == 0) {
        return 0;
    }
    
    return -1;
}

/**
 * Perform switch_root:
 * - chdir to new root
 * - mount --move old root mount points
 * - use pivot_root or chroot + exec
 */
static int switch_root(const char *new_root, const char *init_path) {
    if (chdir(new_root) == -1) {
        write_str("chdir to newroot failed\n");
        return -1;
    }
    
    /* Mount procfs and sysfs in new root */
    mkdir("proc", 0755);
    mkdir("sys", 0755);
    mkdir("dev", 0755);
    
    mount_fs("proc", "proc", "proc", 0, NULL);
    mount_fs("sysfs", "sys", "sysfs", 0, NULL);
    mount_fs("devtmpfs", "dev", "devtmpfs", 0, NULL);
    
    /* Root now at new_root, change to our prepared init */
    if (chroot(new_root) == -1) {
        write_str("chroot failed\n");
        return -1;
    }
    
    if (chdir("/") == -1) {
        write_str("chdir / failed\n");
        return -1;
    }
    
    /* Execute the real init */
    execl(init_path, "init", NULL);
    write_str("exec init failed\n");
    return -1;
}

/**
 * Emergency shell on failure
 */
static void emergency_shell(void) {
    write_str("\n=== FusionOS Emergency Shell ===\n");
    write_str("Unable to mount root filesystem.\n");
    write_str("Trying to spawn emergency shell...\n\n");
    
    execl("/bin/sh", "sh", NULL);
    write_str("sh: exec failed\n");
    _exit(88);
}

/**
 * Main initramfs init
 */
int main(void) {
    write_str("FusionOS initramfs init starting...\n");
    
    /* Mount /proc */
    if (mount_fs("proc", "/proc", "proc", 0, NULL) != 0) {
        write_str("warning: /proc mount failed\n");
    }
    
    /* Mount /sys */
    if (mount_fs("sysfs", "/sys", "sysfs", 0, NULL) != 0) {
        write_str("warning: /sys mount failed\n");
    }
    
    /* Mount /dev as devtmpfs */
    if (mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") != 0) {
        write_str("warning: /dev mount failed\n");
    }
    
    /* Populate /dev with mdev */
    write_str("Populating /dev...\n");
    if (run_mdev() != 0) {
        write_str("warning: mdev failed\n");
    }
    
    /* Try to find and mount root device */
    write_str("Searching for root device...\n");
    
    const char *root_devices[] = {
        "/dev/vda",     /* QEMU virtio block device */
        "/dev/vda1",    /* QEMU virtio with partition */
        "/dev/sda",     /* SATA/IDE */
        "/dev/sda1",    /* SATA with partition */
        "/dev/sr0",     /* CD-ROM (ISO boot) */
        NULL
    };
    
    int root_found = 0;
    for (int i = 0; root_devices[i] != NULL; i++) {
        write_str("Trying to mount: ");
        write_str(root_devices[i]);
        write_str("\n");
        
        if (try_mount_rootfs(root_devices[i]) == 0) {
            write_str("Successfully mounted root from: ");
            write_str(root_devices[i]);
            write_str("\n");
            root_found = 1;
            break;
        }
    }
    
    if (!root_found) {
        write_str("ERROR: Could not mount any root device!\n");
        emergency_shell();
        return 1;
    }
    
    /* Switch to new root */
    write_str("Switching to new root...\n");
    if (switch_root("/newroot", "/sbin/init") != 0) {
        write_str("ERROR: switch_root failed!\n");
        emergency_shell();
        return 1;
    }
    
    /* Should never reach here if exec succeeds */
    write_str("ERROR: init exec should not return\n");
    emergency_shell();
    return 1;
}
