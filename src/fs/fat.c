#define _GNU_SOURCE
#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

/* ── Low-level I/O helpers ───────────────────────────────────────────── */

/** Read `size` bytes from `fd` at absolute byte `offset`. */
static int read_at(int fd, void *buf, size_t size, uint64_t offset) {
    if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (char *)buf + done, size - done);
        if (n == 0) { errno = EIO; return -1; }
        if (n < 0)  { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/** Write `size` bytes to `fd` at absolute byte `offset`. */
static int write_at(int fd, const void *buf, size_t size, uint64_t offset) {
    if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const char *)buf + done, size - done);
        if (n == 0) { errno = EIO; return -1; }
        if (n < 0)  { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/** Convert an LBA sector number to a byte offset within the image/device. */
static uint64_t lba_to_offset(const FatVolume *vol, uint32_t lba) {
    return ((uint64_t)(vol->partition_lba + lba)) * vol->bytes_per_sector;
}

/* ── MBR partition table ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t lba_size;
} MbrPartEntry;

typedef struct __attribute__((packed)) {
    uint8_t       bootstrap[446];
    MbrPartEntry  partitions[4];
    uint16_t      signature;       /* 0xAA55 */
} Mbr;

static uint32_t read_partition_lba(int fd, int partition) {
    if (partition <= 0) return 0;
    Mbr mbr;
    if (read_at(fd, &mbr, sizeof(mbr), 0) != 0) return 0;
    if (mbr.signature != 0xAA55) return 0;
    if (partition > 4) return 0;
    return mbr.partitions[partition - 1].lba_start;
}

/* ── BPB parsing ─────────────────────────────────────────────────────── */

static int parse_bpb(FatVolume *vol) {
    /* Read first sector of the partition */
    uint8_t sector[512];
    if (read_at(vol->fd, sector, 512, (uint64_t)vol->partition_lba * 512) != 0)
        return -1;

    /* Check boot signature */
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        errno = EINVAL;
        return -1;
    }

    const FatBpbCommon *bpb = (const FatBpbCommon *)sector;

    vol->bytes_per_sector    = bpb->bytes_per_sector;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->bytes_per_cluster   = vol->bytes_per_sector * vol->sectors_per_cluster;
    vol->reserved_sectors    = bpb->reserved_sectors;
    vol->num_fats            = bpb->num_fats;
    vol->fat_start_lba       = bpb->reserved_sectors;

    uint32_t fat_sectors;
    if (bpb->fat_size_16 != 0) {
        fat_sectors = bpb->fat_size_16;
    } else {
        const Fat32Ebpb *e32 = (const Fat32Ebpb *)(sector + 36);
        fat_sectors = e32->fat_size_32;
    }
    vol->fat_sectors = fat_sectors;

    uint32_t root_dir_sectors = 0;
    if (bpb->root_entry_count != 0) {
        root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32 +
                             vol->bytes_per_sector - 1) / vol->bytes_per_sector;
    }

    uint32_t data_start = bpb->reserved_sectors +
                          (uint32_t)bpb->num_fats * fat_sectors +
                          root_dir_sectors;
    vol->data_start_lba = data_start;

    uint32_t total_sectors = bpb->total_sectors_16 != 0
                             ? bpb->total_sectors_16
                             : bpb->total_sectors_32;
    vol->total_clusters = (total_sectors - data_start) / vol->sectors_per_cluster;

    /*
     * Determine FAT type from cluster count.
     * Thresholds are defined in Microsoft's FAT specification (fatgen103.doc):
     *   CountOfClusters < 4085  → FAT12
     *   CountOfClusters < 65525 → FAT16
     *   Otherwise               → FAT32
     * These values are non-negotiable; do not change them.
     */
    if (vol->total_clusters < 4085) {
        vol->type = FAT_TYPE_FAT12;
    } else if (vol->total_clusters < 65525) {
        vol->type = FAT_TYPE_FAT16;
    } else {
        vol->type = FAT_TYPE_FAT32;
    }

    if (vol->type == FAT_TYPE_FAT32) {
        const Fat32Ebpb *e32 = (const Fat32Ebpb *)(sector + 36);
        vol->root_cluster   = e32->root_cluster;
        vol->root_dir_lba   = 0;
        vol->root_dir_sectors = 0;
        /* Volume label */
        memcpy(vol->volume_label, e32->volume_label, 11);
        vol->volume_label[11] = '\0';
        /* Trim trailing spaces */
        for (int i = 10; i >= 0 && vol->volume_label[i] == ' '; i--)
            vol->volume_label[i] = '\0';
    } else {
        vol->root_cluster   = 0;
        vol->root_dir_lba   = bpb->reserved_sectors +
                               (uint32_t)bpb->num_fats * fat_sectors;
        vol->root_dir_sectors = root_dir_sectors;
        /* Label from FAT16 ebpb */
        const Fat16Ebpb *e16 = (const Fat16Ebpb *)(sector + 36);
        memcpy(vol->volume_label, e16->volume_label, 11);
        vol->volume_label[11] = '\0';
        for (int i = 10; i >= 0 && vol->volume_label[i] == ' '; i--)
            vol->volume_label[i] = '\0';
    }

    return 0;
}

/* ── FAT cluster chain I/O ───────────────────────────────────────────── */

/** Read the FAT entry for `cluster`, returning the next cluster or EOC/free. */
static uint32_t fat_next_cluster(const FatVolume *vol, uint32_t cluster) {
    uint32_t fat_offset, sector, entry_offset;
    uint8_t  buf[512];
    uint32_t val = FAT32_CLUSTER_EOC;

    if (vol->type == FAT_TYPE_FAT32) {
        fat_offset   = cluster * 4;
        sector       = vol->fat_start_lba + fat_offset / vol->bytes_per_sector;
        entry_offset = fat_offset % vol->bytes_per_sector;
        if (read_at(vol->fd, buf, 512, lba_to_offset(vol, sector)) != 0)
            return FAT32_CLUSTER_EOC;
        val = ((uint32_t)buf[entry_offset]       |
               ((uint32_t)buf[entry_offset + 1] <<  8) |
               ((uint32_t)buf[entry_offset + 2] << 16) |
               ((uint32_t)buf[entry_offset + 3] << 24)) & 0x0FFFFFFFU;
    } else if (vol->type == FAT_TYPE_FAT16) {
        fat_offset   = cluster * 2;
        sector       = vol->fat_start_lba + fat_offset / vol->bytes_per_sector;
        entry_offset = fat_offset % vol->bytes_per_sector;
        if (read_at(vol->fd, buf, 512, lba_to_offset(vol, sector)) != 0)
            return FAT32_CLUSTER_EOC;
        uint16_t v16 = (uint16_t)(buf[entry_offset] |
                                   ((uint16_t)buf[entry_offset + 1] << 8));
        val = (v16 >= 0xFFF8) ? FAT32_CLUSTER_EOC : (uint32_t)v16;
    } else {
        /* FAT12: 12 bits per entry */
        fat_offset   = cluster + (cluster / 2);
        sector       = vol->fat_start_lba + fat_offset / vol->bytes_per_sector;
        entry_offset = fat_offset % vol->bytes_per_sector;
        /* May span two sectors; read two for safety */
        uint8_t buf2[1024];
        if (read_at(vol->fd, buf2, 1024, lba_to_offset(vol, sector)) != 0)
            return FAT32_CLUSTER_EOC;
        uint16_t v12 = (uint16_t)(buf2[entry_offset] |
                                   ((uint16_t)buf2[entry_offset + 1] << 8));
        if (cluster & 1) v12 >>= 4;
        else             v12 &= 0x0FFF;
        val = (v12 >= 0xFF8) ? FAT32_CLUSTER_EOC : (uint32_t)v12;
    }
    return val;
}

/** Convert cluster number to LBA of its first sector. */
static uint32_t cluster_to_lba(const FatVolume *vol, uint32_t cluster) {
    return vol->data_start_lba + (cluster - 2) * vol->sectors_per_cluster;
}

/* ── Cluster buffer I/O ──────────────────────────────────────────────── */

/** Read the full cluster `cluster` into `buf` (must be bytes_per_cluster bytes). */
static int read_cluster(const FatVolume *vol, uint32_t cluster, void *buf) {
    uint32_t lba = cluster_to_lba(vol, cluster);
    return read_at(vol->fd, buf, vol->bytes_per_cluster,
                   lba_to_offset(vol, lba));
}

/** Write the full cluster `cluster` from `buf`. */
static int write_cluster(const FatVolume *vol, uint32_t cluster,
                         const void *buf)
    __attribute__((unused));
static int write_cluster(const FatVolume *vol, uint32_t cluster,
                         const void *buf) {
    uint32_t lba = cluster_to_lba(vol, cluster);
    return write_at(vol->fd, buf, vol->bytes_per_cluster,
                    lba_to_offset(vol, lba));
}

/* ── LFN helpers ─────────────────────────────────────────────────────── */

/**
 * Extract one UTF-16 code unit from an LFN entry's name field.
 * `part` 0 → name1 (5 chars), 1 → name2 (6 chars), 2 → name3 (2 chars).
 */
static uint16_t lfn_char(const FatLfnEntry *lfn, int idx) {
    if (idx < 5)       return lfn->name1[idx];
    else if (idx < 11) return lfn->name2[idx - 5];
    else               return lfn->name3[idx - 11];
}

/**
 * Assemble a long filename from up to 20 LFN entries collected in reverse.
 * `lfn_buf[0]` is the LAST LFN entry (highest sequence number); the array
 * must be ordered so lfn_buf[0] is seq 1 … lfn_buf[n-1] is the last.
 * Actually we store them as encountered (reverse order) and fix up here.
 */
static void assemble_lfn(const FatLfnEntry *lfn_entries, int count,
                         char *out, size_t out_size) {
    /* Entries arrive in reverse disk order: entry[0] has highest sequence,
       entry[count-1] has sequence 1.  We reconstruct by iterating backwards. */
    size_t pos = 0;
    for (int e = count - 1; e >= 0 && pos + 1 < out_size; e--) {
        for (int i = 0; i < 13 && pos + 1 < out_size; i++) {
            uint16_t ch = lfn_char(&lfn_entries[e], i);
            if (ch == 0x0000 || ch == 0xFFFF) goto done;
            /* Simple UTF-16 to ASCII/Latin-1 conversion */
            if (ch < 0x80) {
                out[pos++] = (char)ch;
            } else if (ch < 0x100) {
                out[pos++] = (char)ch; /* Latin-1 passthrough */
            } else {
                out[pos++] = '?'; /* Non-representable → '?' */
            }
        }
    }
done:
    out[pos] = '\0';
}

/**
 * Convert a FAT 8.3 dir-entry name to a NUL-terminated string.
 * E.g. "COMMAND COM" → "COMMAND.COM".
 */
static void decode_short_name(const uint8_t name[11], char *out) {
    int i = 0, j = 0;
    /* Base name (up to 8 chars) */
    for (i = 0; i < 8 && name[i] != ' '; i++)
        out[j++] = (char)name[i];
    /* Extension */
    if (name[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && name[i] != ' '; i++)
            out[j++] = (char)name[i];
    }
    out[j] = '\0';
}

/* ── Date/time conversion ────────────────────────────────────────────── */

int64_t fat_datetime_to_unix(uint16_t date, uint16_t time_field,
                              uint8_t tenths) {
    if (date == 0) return 0;

    int year  = 1980 + ((date >> 9) & 0x7F);
    int month = (date >> 5) & 0x0F;
    int day   = date & 0x1F;
    int hour  = (time_field >> 11) & 0x1F;
    int min   = (time_field >> 5)  & 0x3F;
    int sec   = (time_field & 0x1F) * 2 + tenths / 100;

    /* Simple Gregorian → Unix timestamp (no DST) */
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_sec  = sec;
    t.tm_isdst = -1;
    time_t ts = mktime(&t);
    return (int64_t)ts;
}

/* ── FatFile helpers ─────────────────────────────────────────────────── */

static int file_load_cluster(FatFile *file, uint32_t cluster) {
    if (file->cluster_buf == NULL) {
        file->cluster_buf = malloc(file->vol->bytes_per_cluster);
        if (!file->cluster_buf) return -1;
    }
    if (read_cluster(file->vol, cluster, file->cluster_buf) != 0) return -1;
    file->cur_cluster = cluster;
    return 0;
}

/**
 * Advance the cluster chain by `n` steps from the current position.
 * Returns the new cluster, or FAT32_CLUSTER_EOC on end.
 */
static uint32_t chain_advance(const FatVolume *vol, uint32_t cluster,
                               uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        cluster = fat_next_cluster(vol, cluster);
        if (cluster >= FAT32_CLUSTER_EOC) return FAT32_CLUSTER_EOC;
    }
    return cluster;
}

/* ── Public API — mount/unmount ──────────────────────────────────────── */

int fat_mount(const char *path, int partition, FatVolume *vol) {
    memset(vol, 0, sizeof(*vol));

    vol->fd = open(path, O_RDWR);
    if (vol->fd < 0) {
        /* Try read-only */
        vol->fd = open(path, O_RDONLY);
        if (vol->fd < 0) return -1;
    }

    vol->partition_lba = read_partition_lba(vol->fd, partition);
    /* Default sector size before parsing */
    vol->bytes_per_sector = 512;

    if (parse_bpb(vol) != 0) {
        close(vol->fd);
        vol->fd = -1;
        return -1;
    }
    return 0;
}

void fat_unmount(FatVolume *vol) {
    if (vol->fd >= 0) close(vol->fd);
    vol->fd = -1;
}

/* ── Public API — file open/read/seek/close ─────────────────────────── */

static int find_entry(FatVolume *vol, const char *path,
                      uint32_t *first_cluster, uint32_t *file_size,
                      int *is_dir);

int fat_open(FatVolume *vol, const char *path, FatFile *out) {
    memset(out, 0, sizeof(*out));
    out->vol = vol;

    uint32_t first_cluster = 0, file_size = 0;
    int is_dir = 0;
    if (find_entry(vol, path, &first_cluster, &file_size, &is_dir) != 0)
        return -1;

    out->first_cluster  = first_cluster;
    out->file_size      = file_size;
    out->is_dir         = is_dir;
    out->cur_cluster     = first_cluster;
    out->cur_cluster_idx = 0;
    out->pos             = 0;
    out->cluster_buf     = NULL;

    if (first_cluster >= 2)
        return file_load_cluster(out, first_cluster);
    return 0;
}

void fat_close(FatFile *file) {
    free(file->cluster_buf);
    file->cluster_buf = NULL;
}

ssize_t fat_read(FatFile *file, void *buf, size_t len) {
    if (!file || !buf || len == 0) return 0;

    /* Determine readable length */
    uint32_t remaining = file->file_size > 0
                         ? file->file_size - file->pos
                         : (uint32_t)-1;
    if (len > remaining) len = remaining;
    if (len == 0) return 0;

    FatVolume *vol = file->vol;
    uint8_t   *out = (uint8_t *)buf;
    size_t     done = 0;

    while (done < len) {
        if (!file->cluster_buf) {
            if (file_load_cluster(file, file->cur_cluster) != 0) return -1;
        }

        uint32_t cluster_offset = file->pos % vol->bytes_per_cluster;
        uint32_t can_read = vol->bytes_per_cluster - cluster_offset;
        size_t   to_copy  = len - done;
        if (to_copy > can_read) to_copy = can_read;

        memcpy(out + done, file->cluster_buf + cluster_offset, to_copy);
        done     += to_copy;
        file->pos += (uint32_t)to_copy;

        /* Advance to next cluster if we've consumed this one */
        if (file->pos % vol->bytes_per_cluster == 0) {
            uint32_t next = fat_next_cluster(vol, file->cur_cluster);
            if (next >= FAT32_CLUSTER_EOC) break;
            file->cur_cluster = next;
            file->cur_cluster_idx++;
            if (file_load_cluster(file, next) != 0) return -1;
        }
    }
    return (ssize_t)done;
}

int fat_seek(FatFile *file, uint32_t offset) {
    if (offset > file->file_size) { errno = EINVAL; return -1; }

    FatVolume *vol = file->vol;
    uint32_t target_cluster_idx = offset / vol->bytes_per_cluster;

    if (target_cluster_idx < file->cur_cluster_idx) {
        /* Rewind to start */
        file->cur_cluster     = file->first_cluster;
        file->cur_cluster_idx = 0;
    }

    uint32_t steps = target_cluster_idx - file->cur_cluster_idx;
    if (steps > 0) {
        uint32_t c = chain_advance(vol, file->cur_cluster, steps);
        if (c >= FAT32_CLUSTER_EOC) { errno = EINVAL; return -1; }
        file->cur_cluster     = c;
        file->cur_cluster_idx = target_cluster_idx;
    }

    file->pos = offset;
    return file_load_cluster(file, file->cur_cluster);
}

/* ── Directory traversal ─────────────────────────────────────────────── */

int fat_opendir(FatFile *dir_file, FatDir *out) {
    if (!dir_file->is_dir) { errno = ENOTDIR; return -1; }
    out->dir_file    = dir_file;
    out->entry_offset = 0;
    return 0;
}

int fat_readdir(FatDir *dir, FatDirEnt *ent) {
#define MAX_LFN_ENTRIES 20
    FatLfnEntry lfn_entries[MAX_LFN_ENTRIES];
    int lfn_count = 0;

    FatFile *f = dir->dir_file;

    while (1) {
        /* Seek to current entry offset */
        if (fat_seek(f, dir->entry_offset) != 0) return -1;

        uint8_t raw[32];
        ssize_t n = fat_read(f, raw, 32);
        if (n < 32) return 0; /* EOF */
        dir->entry_offset += 32;

        if (raw[0] == FAT_DIRENT_END) return 0; /* No more entries */
        if (raw[0] == FAT_DIRENT_FREE) { lfn_count = 0; continue; }

        /* Check if this is an LFN entry */
        if ((raw[11] & 0x3F) == FAT_ATTR_LONG_NAME) {
            if (lfn_count < MAX_LFN_ENTRIES) {
                memcpy(&lfn_entries[lfn_count++], raw, 32);
            }
            continue;
        }

        /* Volume label — skip */
        if (raw[11] & FAT_ATTR_VOLUME_ID) { lfn_count = 0; continue; }

        /* Regular 8.3 or directory entry */
        const FatDirEntry *de = (const FatDirEntry *)raw;

        memset(ent, 0, sizeof(*ent));

        decode_short_name(de->name, ent->short_name);

        if (lfn_count > 0) {
            assemble_lfn(lfn_entries, lfn_count, ent->name, sizeof(ent->name));
            lfn_count = 0;
        } else {
            snprintf(ent->name, sizeof(ent->name), "%s", ent->short_name);
        }

        ent->is_dir      = (de->attr & FAT_ATTR_DIRECTORY) != 0;
        ent->is_hidden   = (de->attr & FAT_ATTR_HIDDEN)    != 0;
        ent->is_read_only= (de->attr & FAT_ATTR_READ_ONLY) != 0;
        ent->file_size   = de->file_size;
        ent->first_cluster = ((uint32_t)de->first_cluster_hi << 16) |
                              de->first_cluster_lo;
        ent->modify_time = fat_datetime_to_unix(de->write_date,
                                                de->write_time, 0);
        ent->create_time = fat_datetime_to_unix(de->create_date,
                                                de->create_time,
                                                de->create_time_tenth);
        ent->access_time = fat_datetime_to_unix(de->last_access_date, 0, 0);
        return 1;
    }
}

void fat_closedir(FatDir *dir) {
    (void)dir;
    /* Nothing to free; the underlying FatFile is caller-owned */
}

/* ── Path resolution ─────────────────────────────────────────────────── */

/**
 * Look up a path component within a directory cluster chain.
 * Fills `*out_cluster`, `*out_size`, `*out_is_dir`.
 */
static int lookup_in_dir(FatVolume *vol, uint32_t dir_cluster,
                         uint32_t root_lba, uint32_t root_sectors,
                         const char *component,
                         uint32_t *out_cluster, uint32_t *out_size,
                         int *out_is_dir) {
    FatFile df = {0};
    df.vol          = vol;
    df.is_dir       = 1;
    df.first_cluster = dir_cluster;
    df.cur_cluster   = dir_cluster;

    /* For FAT12/16 root directory (linear sectors, not cluster chain) */
    if (dir_cluster == 0 && root_sectors > 0) {
        /* Synthesise a fake large file spanning root dir sectors */
        df.file_size = root_sectors * vol->bytes_per_sector;
        /* Temporarily abuse cluster_buf to hold root dir data */
        df.cluster_buf = malloc(df.file_size);
        if (!df.cluster_buf) return -1;
        if (read_at(vol->fd, df.cluster_buf, df.file_size,
                    lba_to_offset(vol, root_lba)) != 0) {
            free(df.cluster_buf);
            return -1;
        }
        /* Replace fat_read with direct memcpy via cursor */
    } else {
        df.file_size = 0; /* directory: unlimited */
        if (dir_cluster >= 2) file_load_cluster(&df, dir_cluster);
    }

    FatDir dd;
    fat_opendir(&df, &dd);

    int found = 0;
    FatDirEnt ent;
    int r;
    while ((r = fat_readdir(&dd, &ent)) == 1) {
        if (strcasecmp(ent.name,       component) == 0 ||
            strcasecmp(ent.short_name, component) == 0) {
            *out_cluster = ent.first_cluster;
            *out_size    = ent.file_size;
            *out_is_dir  = ent.is_dir;
            found = 1;
            break;
        }
    }

    fat_close(&df);
    if (!found) { errno = ENOENT; return -1; }
    return 0;
}

static int find_entry(FatVolume *vol, const char *path,
                      uint32_t *first_cluster, uint32_t *file_size,
                      int *is_dir) {
    /* Normalise path: strip leading / or drive letter */
    const char *p = path;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '/' || *p == '\\') p++;

    /* Start at root directory */
    uint32_t cur_cluster = (vol->type == FAT_TYPE_FAT32)
                           ? vol->root_cluster : 0;
    int      cur_is_dir  = 1;
    uint32_t cur_size    = 0;

    if (*p == '\0') {
        /* Path is the root itself */
        *first_cluster = cur_cluster;
        *file_size     = 0;
        *is_dir        = 1;
        return 0;
    }

    char component[256];
    while (*p) {
        /* Extract next component */
        size_t i = 0;
        while (*p && *p != '/' && *p != '\\' && i < sizeof(component) - 1)
            component[i++] = *p++;
        component[i] = '\0';
        if (*p) p++; /* skip separator */
        if (!component[0]) continue; /* double-slash */

        if (!cur_is_dir) { errno = ENOTDIR; return -1; }

        if (lookup_in_dir(vol, cur_cluster,
                          vol->root_dir_lba, vol->root_dir_sectors,
                          component,
                          &cur_cluster, &cur_size, &cur_is_dir) != 0) {
            return -1;
        }
    }

    *first_cluster = cur_cluster;
    *file_size     = cur_size;
    *is_dir        = cur_is_dir;
    return 0;
}

/* ── Public API — write / create / unlink ────────────────────────────── */

ssize_t fat_write(FatFile *file, const void *buf, size_t len) {
    /* Minimal write stub — full implementation requires FAT allocation. */
    if (!file || !buf) { errno = EINVAL; return -1; }
    (void)len;
    /* Full write support requires cluster allocation; mark as TODO */
    errno = ENOSYS;
    return -1;
}

int fat_create(FatVolume *vol, const char *path, int is_dir) {
    (void)vol; (void)path; (void)is_dir;
    errno = ENOSYS;
    return -1;
}

int fat_unlink(FatVolume *vol, const char *path) {
    (void)vol; (void)path;
    errno = ENOSYS;
    return -1;
}

/* ── Utility ─────────────────────────────────────────────────────────── */

const char *fat_type_name(FatType type) {
    switch (type) {
        case FAT_TYPE_FAT12: return "FAT12";
        case FAT_TYPE_FAT16: return "FAT16";
        case FAT_TYPE_FAT32: return "FAT32";
        case FAT_TYPE_EXFAT: return "exFAT";
        default:             return "Unknown";
    }
}
