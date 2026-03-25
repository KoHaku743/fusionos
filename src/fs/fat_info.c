#define _GNU_SOURCE
#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/*
 * fat-info -- FusionOS FAT volume inspector
 * Usage: fat-info <image_or_device> [partition]
 *
 * Displays filesystem metadata and lists the root directory.
 */

static void list_dir(FatVolume *vol, uint32_t cluster, const char *prefix,
                     int depth) {
    if (depth > 4) return; /* prevent infinite recursion on corrupt images */

    FatFile df = {0};
    df.vol          = vol;
    df.is_dir       = 1;
    df.first_cluster = cluster;
    df.cur_cluster   = cluster;
    df.file_size     = 0;

    /* For FAT12/16 root: fake a large file size */
    if (cluster == 0 && vol->root_dir_sectors > 0) {
        df.file_size = vol->root_dir_sectors * vol->bytes_per_sector;
        df.cluster_buf = malloc(df.file_size);
        if (!df.cluster_buf) return;
        /* Read root dir sectors directly */
        if (lseek(vol->fd, (off_t)(vol->root_dir_lba * vol->bytes_per_sector),
                  SEEK_SET) < 0 ||
            read(vol->fd, df.cluster_buf, df.file_size) < (ssize_t)df.file_size) {
            free(df.cluster_buf);
            return;
        }
    } else {
        /* Load first cluster */
        if (cluster >= 2) {
            df.cluster_buf = malloc(vol->bytes_per_cluster);
            if (!df.cluster_buf) return;
            /* We call fat_seek to load it */
        }
    }

    FatDir dd;
    fat_opendir(&df, &dd);

    FatDirEnt ent;
    int r;
    while ((r = fat_readdir(&dd, &ent)) == 1) {
        /* Skip . and .. */
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s%s%s",
                 prefix, prefix[0] ? "\\" : "", ent.name);

        time_t mt = (time_t)ent.modify_time;
        char   ts[32] = "                ";
        if (mt > 0) {
            struct tm *tm = localtime(&mt);
            if (tm) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm);
        }

        if (ent.is_dir) {
            printf("  %-40s  <DIR>          %s\n", path, ts);
            list_dir(vol, ent.first_cluster, path, depth + 1);
        } else {
            printf("  %-40s  %12u  %s\n", path, ent.file_size, ts);
        }
    }

    fat_close(&df);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <image_or_device> [partition]\n", prog);
    fprintf(stderr, "  partition  MBR partition number 1-4 (default: 0 = whole disk)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(argv[0]); return 1; }

    const char *path = argv[1];
    int partition = (argc > 2) ? atoi(argv[2]) : 0;

    FatVolume vol;
    if (fat_mount(path, partition, &vol) != 0) {
        perror("fat_mount");
        fprintf(stderr, "Could not mount FAT volume: %s\n", path);
        return 1;
    }

    printf("FAT Volume Information\n");
    printf("======================\n");
    printf("  Image/Device   : %s\n", path);
    printf("  Partition      : %d\n", partition);
    printf("  Filesystem type: %s\n", fat_type_name(vol.type));
    printf("  Volume label   : %s\n", vol.volume_label[0] ? vol.volume_label : "(none)");
    printf("  Bytes/sector   : %u\n", vol.bytes_per_sector);
    printf("  Sectors/cluster: %u\n", vol.sectors_per_cluster);
    printf("  Bytes/cluster  : %u\n", vol.bytes_per_cluster);
    printf("  Total clusters : %u\n", vol.total_clusters);
    printf("  FAT copies     : %u\n", vol.num_fats);
    if (vol.type == FAT_TYPE_FAT32)
        printf("  Root cluster   : %u\n", vol.root_cluster);
    printf("\nDirectory listing:\n\n");

    uint32_t root = (vol.type == FAT_TYPE_FAT32) ? vol.root_cluster : 0;
    list_dir(&vol, root, "", 0);
    printf("\n");

    fat_unmount(&vol);
    return 0;
}
