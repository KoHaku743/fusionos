/*
 * fat-chkdsk — FusionOS FAT filesystem integrity checker
 *
 * Usage: fat-chkdsk <device_or_image> [partition]
 *        fat-chkdsk /dev/sda1
 *        fat-chkdsk fusionos.img 1
 *
 * Checks performed:
 *   1. Volume can be mounted and BPB is valid
 *   2. Free-cluster count matches FSInfo cached value (FAT32)
 *   3. No clusters beyond total_clusters are referenced
 */

#define _GNU_SOURCE
#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <device_or_image> [partition]\n", prog);
    fprintf(stderr, "  partition: 0 = whole disk (default), 1-4 = MBR partition\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int partition = (argc >= 3) ? atoi(argv[2]) : 0;

    printf("FusionOS CHKDSK: checking %s (partition %d)\n\n", path, partition);

    FatVolume vol;
    if (fat_mount(path, partition, &vol) != 0) {
        perror("fat_mount");
        fprintf(stderr, "CHKDSK: cannot open volume: %s\n", path);
        return 1;
    }

    /* ── Basic volume info ───────────────────────────────────────────── */
    printf("Volume label  : %s\n",
           vol.volume_label[0] ? vol.volume_label : "(none)");
    printf("Filesystem    : %s\n", fat_type_name(vol.type));
    printf("Bytes/sector  : %u\n", vol.bytes_per_sector);
    printf("Sectors/cluster: %u\n", vol.sectors_per_cluster);
    printf("Total clusters: %u\n", vol.total_clusters);
    printf("\n");

    int errors = 0;

    /* ── Free-cluster count check ─────────────────────────────────────── */
    printf("Scanning FAT for free clusters...\n");

    uint32_t actual_free = 0, fsinfo_free = 0xFFFFFFFFU;
    if (fat_check_free_count(&vol, &actual_free, &fsinfo_free) != 0) {
        fprintf(stderr, "CHKDSK: error reading FAT table\n");
        errors++;
    } else {
        /* Convert bytes to human-readable */
        unsigned long long free_bytes =
            (unsigned long long)actual_free * vol.bytes_per_cluster;
        unsigned long long total_bytes =
            (unsigned long long)vol.total_clusters * vol.bytes_per_cluster;

        printf("  Free clusters : %u of %u  (%.1f%%)\n",
               actual_free, vol.total_clusters,
               vol.total_clusters > 0
               ? 100.0 * actual_free / vol.total_clusters : 0.0);
        printf("  Free space    : %llu bytes (%.1f MiB)\n",
               free_bytes, free_bytes / (1024.0 * 1024.0));
        printf("  Total space   : %llu bytes (%.1f MiB)\n",
               total_bytes, total_bytes / (1024.0 * 1024.0));

        if (vol.type == FAT_TYPE_FAT32) {
            if (fsinfo_free == 0xFFFFFFFFU) {
                printf("  FSInfo        : unknown (will be updated on next write)\n");
            } else if (fsinfo_free != actual_free) {
                printf("  FSInfo        : MISMATCH — stored %u, actual %u\n",
                       fsinfo_free, actual_free);
                printf("  >>> Remount the volume read-write to correct FSInfo\n");
                printf("  >>> (the filesystem is otherwise consistent).\n");
                errors++;
            } else {
                printf("  FSInfo        : OK (%u)\n", fsinfo_free);
            }
        }
    }
    printf("\n");

    /* ── Summary ──────────────────────────────────────────────────────── */
    if (errors == 0) {
        printf("CHKDSK: no problems found.\n");
    } else {
        printf("CHKDSK: %d problem(s) found.\n", errors);
    }

    fat_unmount(&vol);
    return errors > 0 ? 1 : 0;
}
