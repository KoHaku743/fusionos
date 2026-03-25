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
        /* FSInfo */
        vol->fs_info_sector = e32->fs_info;
        vol->free_cluster_count = 0xFFFFFFFFU;
        vol->next_free_cluster  = 0xFFFFFFFFU;
        if (vol->fs_info_sector != 0 && vol->fs_info_sector != 0xFFFF) {
            FatFsInfo fi;
            uint64_t fi_off = (uint64_t)(vol->partition_lba +
                                         vol->fs_info_sector) * 512;
            if (read_at(vol->fd, &fi, sizeof(fi), fi_off) == 0 &&
                fi.lead_sig   == 0x41615252U &&
                fi.struct_sig == 0x61417272U) {
                vol->free_cluster_count = fi.free_count;
                vol->next_free_cluster  = fi.next_free;
            }
        }
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

/* ── Sector-level directory iterator ────────────────────────────────── */

/*
 * SecIter walks a directory one sector at a time, abstracting over both
 * FAT12/16 root directories (fixed LBA range) and FAT32 cluster chains.
 * The caller reads `it.sector[]` and `it.cur_lba` after each seciter_next().
 */
typedef struct {
    FatVolume *vol;
    /* Root-dir mode (FAT12/16) */
    uint32_t  root_lba;
    uint32_t  root_sectors;
    uint32_t  root_cur;        /* next sector index to yield */
    /* Cluster-chain mode */
    uint32_t  cur_cluster;
    uint32_t  sec_in_cluster;  /* next sector within cur_cluster */
    int       is_root;         /* 1 = root-dir mode */
    /* Output */
    uint32_t  cur_lba;
    uint8_t   sector[4096];    /* current sector data (max sector size) */
    int       at_end;
} SecIter;

static void seciter_init(SecIter *it, FatVolume *vol, uint32_t dir_cluster) {
    memset(it, 0, sizeof(*it));
    it->vol = vol;
    if (dir_cluster == 0 && vol->root_dir_sectors > 0) {
        it->is_root      = 1;
        it->root_lba     = vol->root_dir_lba;
        it->root_sectors = vol->root_dir_sectors;
    } else {
        it->is_root     = 0;
        it->cur_cluster = dir_cluster;
    }
}

/* Returns 1 if sector loaded into it->sector[], 0 at end, -1 on error. */
static int seciter_next(SecIter *it) {
    if (it->at_end) return 0;

    if (it->is_root) {
        if (it->root_cur >= it->root_sectors) { it->at_end = 1; return 0; }
        it->cur_lba = it->root_lba + it->root_cur++;
    } else {
        if (it->cur_cluster < 2 || it->cur_cluster >= FAT32_CLUSTER_EOC) {
            it->at_end = 1;
            return 0;
        }
        it->cur_lba = cluster_to_lba(it->vol, it->cur_cluster) +
                      it->sec_in_cluster;
        it->sec_in_cluster++;
        if (it->sec_in_cluster >= it->vol->sectors_per_cluster) {
            it->sec_in_cluster = 0;
            it->cur_cluster = fat_next_cluster(it->vol, it->cur_cluster);
        }
    }

    if (read_at(it->vol->fd, it->sector, it->vol->bytes_per_sector,
                lba_to_offset(it->vol, it->cur_lba)) != 0)
        return -1;
    return 1;
}

/* ── Low-level directory-entry finder ───────────────────────────────── */

/*
 * Scan a directory for an entry by name (8.3 short name comparison) or for
 * the first free slot.  Collects up to MAX_LFN_ENTRIES preceding LFN entries
 * so they can also be deleted on unlink.
 *
 * dir_cluster: first cluster of directory (0 = FAT12/16 root).
 * target_name: decoded name to match (e.g. "GAME.EXE"), or NULL.
 * want_free  : 1 = return the first free/deleted slot instead.
 *
 * On success (returns 1):
 *   *match_lba / *match_off   – position of the matched 8.3 entry.
 *   match_raw[32]             – raw bytes of the matched entry.
 *   lfn_lba / lfn_off / *lfn_cnt – positions of preceding LFN entries.
 *
 * Returns 0 if not found, -1 on error.
 */
#define DIRSCAN_MAX_LFN 20
static int dirscan_find(FatVolume *vol, uint32_t dir_cluster,
                        const char *target_name, int want_free,
                        uint32_t *match_lba, uint32_t *match_off,
                        uint8_t match_raw[32],
                        uint32_t lfn_lba[DIRSCAN_MAX_LFN],
                        uint32_t lfn_off[DIRSCAN_MAX_LFN],
                        int *lfn_cnt) {
    SecIter it;
    seciter_init(&it, vol, dir_cluster);

    uint32_t bps   = vol->bytes_per_sector;
    uint32_t epb   = bps / 32;          /* entries per sector */

    /* Rolling LFN accumulator */
    uint32_t pending_lfn_lba[DIRSCAN_MAX_LFN];
    uint32_t pending_lfn_off[DIRSCAN_MAX_LFN];
    int      pending_lfn_cnt = 0;

    int r;
    while ((r = seciter_next(&it)) == 1) {
        for (uint32_t i = 0; i < epb; i++) {
            uint32_t  off = i * 32;
            uint8_t  *e   = it.sector + off;

            if (e[0] == FAT_DIRENT_END) {
                /* End marker — return it as free slot if wanted */
                if (want_free) {
                    *match_lba = it.cur_lba;
                    *match_off = off;
                    if (match_raw) memset(match_raw, 0, 32);
                    if (lfn_cnt) *lfn_cnt = 0;
                    return 1;
                }
                return 0;
            }

            /* LFN entry — accumulate position */
            if ((e[11] & 0x3F) == FAT_ATTR_LONG_NAME) {
                if (pending_lfn_cnt < DIRSCAN_MAX_LFN) {
                    pending_lfn_lba[pending_lfn_cnt] = it.cur_lba;
                    pending_lfn_off[pending_lfn_cnt] = off;
                    pending_lfn_cnt++;
                }
                continue;
            }

            /* Volume ID — skip, reset LFN accumulator */
            if (e[11] & FAT_ATTR_VOLUME_ID) { pending_lfn_cnt = 0; continue; }

            /* Free / deleted slot */
            if (e[0] == FAT_DIRENT_FREE) {
                if (want_free) {
                    *match_lba = it.cur_lba;
                    *match_off = off;
                    if (match_raw) memcpy(match_raw, e, 32);
                    if (lfn_cnt) *lfn_cnt = 0;
                    return 1;
                }
                pending_lfn_cnt = 0;
                continue;
            }

            /* Real 8.3 entry — check for name match */
            if (target_name) {
                char sname[13];
                decode_short_name(e, sname);
                /* Also try matching the LFN assembled name */
                FatLfnEntry lfn_entries[DIRSCAN_MAX_LFN];
                int n = (pending_lfn_cnt < DIRSCAN_MAX_LFN)
                        ? pending_lfn_cnt : DIRSCAN_MAX_LFN;
                /* We stored them in order encountered (forward); assemble_lfn
                   expects reverse order — pass as-is, it reverses internally */
                /* Copy pending LFN raw bytes into lfn_entries */
                /* Actually assemble_lfn wants them in encounter order —
                   let's just check short name for now */
                (void)lfn_entries; (void)n;

                if (strcasecmp(sname, target_name) == 0) {
                    *match_lba = it.cur_lba;
                    *match_off = off;
                    if (match_raw) memcpy(match_raw, e, 32);
                    if (lfn_cnt && lfn_lba && lfn_off) {
                        int copy = pending_lfn_cnt < DIRSCAN_MAX_LFN
                                   ? pending_lfn_cnt : DIRSCAN_MAX_LFN;
                        for (int k = 0; k < copy; k++) {
                            lfn_lba[k] = pending_lfn_lba[k];
                            lfn_off[k] = pending_lfn_off[k];
                        }
                        *lfn_cnt = copy;
                    }
                    return 1;
                }
            }
            pending_lfn_cnt = 0; /* reset after non-matching real entry */
        }
    }
    return (r < 0) ? -1 : 0;
}

/* ── FAT table write helpers ─────────────────────────────────────────── */

/*
 * Write a FAT entry for `cluster` to all FAT copies.
 * For FAT32 the high 4 bits of the existing entry are preserved (reserved).
 */
static int fat_write_fat_entry(FatVolume *vol, uint32_t cluster,
                                uint32_t value) {
    if (vol->type != FAT_TYPE_FAT32 && vol->type != FAT_TYPE_FAT16)
        return 0; /* FAT12 write not supported */

    for (uint32_t fatnum = 0; fatnum < vol->num_fats; fatnum++) {
        uint8_t buf[512];

        if (vol->type == FAT_TYPE_FAT32) {
            uint32_t fat_offset   = cluster * 4;
            uint32_t sector       = vol->fat_start_lba
                                    + fat_offset / vol->bytes_per_sector
                                    + fatnum * vol->fat_sectors;
            uint32_t entry_offset = fat_offset % vol->bytes_per_sector;
            uint64_t off = lba_to_offset(vol, sector);

            if (read_at(vol->fd, buf, 512, off) != 0) return -1;
            /* Preserve high 4 reserved bits */
            uint32_t existing = ((uint32_t)buf[entry_offset]       |
                                 ((uint32_t)buf[entry_offset+1] <<  8) |
                                 ((uint32_t)buf[entry_offset+2] << 16) |
                                 ((uint32_t)buf[entry_offset+3] << 24)) & 0xF0000000U;
            uint32_t v = (value & 0x0FFFFFFFU) | existing;
            buf[entry_offset]   = (uint8_t)(v & 0xFF);
            buf[entry_offset+1] = (uint8_t)((v >>  8) & 0xFF);
            buf[entry_offset+2] = (uint8_t)((v >> 16) & 0xFF);
            buf[entry_offset+3] = (uint8_t)((v >> 24) & 0xFF);
            if (write_at(vol->fd, buf, 512, off) != 0) return -1;

        } else { /* FAT16 */
            uint32_t fat_offset   = cluster * 2;
            uint32_t sector       = vol->fat_start_lba
                                    + fat_offset / vol->bytes_per_sector
                                    + fatnum * vol->fat_sectors;
            uint32_t entry_offset = fat_offset % vol->bytes_per_sector;
            uint64_t off = lba_to_offset(vol, sector);

            if (read_at(vol->fd, buf, 512, off) != 0) return -1;
            uint16_t v16 = (uint16_t)(value & 0xFFFF);
            buf[entry_offset]   = (uint8_t)(v16 & 0xFF);
            buf[entry_offset+1] = (uint8_t)((v16 >> 8) & 0xFF);
            if (write_at(vol->fd, buf, 512, off) != 0) return -1;
        }
    }
    return 0;
}

/*
 * Allocate a free cluster, link it after `prev_cluster` (0 = no link),
 * and return the new cluster number.  Returns 0 on ENOSPC or error.
 */
static uint32_t fat_alloc_cluster(FatVolume *vol, uint32_t prev_cluster) {
    if (vol->type != FAT_TYPE_FAT32 && vol->type != FAT_TYPE_FAT16) {
        errno = ENOSYS;
        return 0;
    }

    /* Start scan from FSInfo hint or cluster 2 */
    uint32_t start = (vol->next_free_cluster >= 2 &&
                      vol->next_free_cluster < vol->total_clusters + 2)
                     ? vol->next_free_cluster : 2;

    for (uint32_t c = start; c < vol->total_clusters + 2; c++) {
        uint32_t entry = fat_next_cluster(vol, c);
        if (entry == FAT32_CLUSTER_FREE) {
            /* Mark new cluster as EOC */
            if (fat_write_fat_entry(vol, c, FAT32_CLUSTER_EOC) != 0) return 0;
            /* Link previous cluster to this one */
            if (prev_cluster >= 2)
                if (fat_write_fat_entry(vol, prev_cluster, c) != 0) return 0;
            /* Update hints */
            vol->next_free_cluster = c + 1;
            if (vol->free_cluster_count != 0xFFFFFFFFU &&
                vol->free_cluster_count > 0)
                vol->free_cluster_count--;
            return c;
        }
    }
    errno = ENOSPC;
    return 0;
}

/*
 * Walk the cluster chain from `first_cluster` and free every entry.
 */
static int fat_free_cluster_chain(FatVolume *vol, uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC) {
        uint32_t next = fat_next_cluster(vol, cluster);
        if (fat_write_fat_entry(vol, cluster, FAT32_CLUSTER_FREE) != 0)
            return -1;
        if (vol->free_cluster_count != 0xFFFFFFFFU)
            vol->free_cluster_count++;
        cluster = next;
    }
    return 0;
}

/*
 * Persist the cached free_cluster_count / next_free_cluster to the FSInfo
 * sector.  No-op if vol->fs_info_sector == 0.
 */
static int fat_update_fsinfo(FatVolume *vol) {
    if (vol->fs_info_sector == 0 || vol->type != FAT_TYPE_FAT32) return 0;

    FatFsInfo fi;
    uint64_t  off = lba_to_offset(vol, vol->fs_info_sector);
    if (read_at(vol->fd, &fi, sizeof(fi), off) != 0) return -1;

    fi.free_count = vol->free_cluster_count;
    fi.next_free  = vol->next_free_cluster;

    if (write_at(vol->fd, &fi, sizeof(fi), off) != 0) return -1;
    return 0;
}

/* ── Filename helpers ────────────────────────────────────────────────── */

/*
 * Convert a human-readable name to the FAT 11-byte 8.3 raw format
 * (uppercase, space-padded, no dot).  Returns 0 on success, -1 if the
 * name is not a valid 8.3 name.
 */
static int to_fat_name(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (base_len < 1 || base_len > 8) return -1;
    for (int i = 0; i < base_len; i++)
        out[i] = (uint8_t)toupper((unsigned char)name[i]);
    if (dot) {
        const char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len < 1 || ext_len > 3) return -1;
        for (int i = 0; i < ext_len; i++)
            out[8 + i] = (uint8_t)toupper((unsigned char)ext[i]);
    }
    return 0;
}

/*
 * Split a path into parent directory cluster and the final component name.
 * e.g.  "/FOO/BAR.TXT"  →  parent = FOO cluster,  name = "BAR.TXT"
 * Forward declaration of find_entry is below; split_path is used only after
 * find_entry is defined.
 */
static int find_entry(FatVolume *vol, const char *path,
                      uint32_t *first_cluster, uint32_t *file_size,
                      int *is_dir);  /* defined later in this file */

static int find_entry_ex(FatVolume *vol, const char *path,
                         uint32_t *first_cluster, uint32_t *file_size,
                         int *is_dir,
                         uint32_t *dirent_lba_out,
                         uint32_t *dirent_off_out);  /* defined later */

static int split_path(FatVolume *vol, const char *path,
                      uint32_t *parent_cluster, char *basename,
                      size_t basename_size) {
    /* Normalise: strip drive letter, convert \ to / */
    char norm[4096];
    const char *p = path;
    if (p[0] && p[1] == ':') p += 2;
    snprintf(norm, sizeof(norm), "%s", p);
    for (char *q = norm; *q; q++) if (*q == '\\') *q = '/';

    /* Find last slash */
    char *last_slash = strrchr(norm, '/');
    if (!last_slash) {
        /* No slash — file is in root directory */
        *parent_cluster = (vol->type == FAT_TYPE_FAT32) ? vol->root_cluster : 0;
        /* basename_size is always >= 256; FAT names are at most 255 chars */
        size_t blen = strlen(norm);
        if (blen >= basename_size) { errno = ENAMETOOLONG; return -1; }
        memcpy(basename, norm, blen + 1);
        return 0;
    }

    *last_slash = '\0';
    {
        const char *tail = last_slash + 1;
        size_t tlen = strlen(tail);
        if (tlen >= basename_size) { errno = ENAMETOOLONG; return -1; }
        memcpy(basename, tail, tlen + 1);
    }

    /* Resolve the parent path */
    const char *parent_path = norm[0] ? norm : "/";
    uint32_t pc = 0, ps = 0;
    int pi = 0;
    if (find_entry(vol, parent_path, &pc, &ps, &pi) != 0) return -1;
    if (!pi) { errno = ENOTDIR; return -1; }
    *parent_cluster = pc;
    return 0;
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

int fat_open(FatVolume *vol, const char *path, FatFile *out) {
    memset(out, 0, sizeof(*out));
    out->vol = vol;

    uint32_t first_cluster = 0, file_size = 0;
    int is_dir = 0;
    uint32_t dlba = 0, doff = 0;
    if (find_entry_ex(vol, path, &first_cluster, &file_size, &is_dir,
                      &dlba, &doff) != 0)
        return -1;

    out->first_cluster   = first_cluster;
    out->file_size       = file_size;
    out->is_dir          = is_dir;
    out->cur_cluster     = first_cluster;
    out->cur_cluster_idx = 0;
    out->pos             = 0;
    out->cluster_buf     = NULL;
    out->dirent_lba      = dlba;
    out->dirent_off      = doff;
    out->buf_dirty       = 0;

    /* Pre-find last cluster for append-friendly writes */
    out->last_cluster = first_cluster;
    if (first_cluster >= 2) {
        uint32_t c = first_cluster;
        uint32_t next;
        while ((next = fat_next_cluster(vol, c)) < FAT32_CLUSTER_EOC)
            c = next;
        out->last_cluster = c;
        return file_load_cluster(out, first_cluster);
    }
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
 * Optionally fills `*out_dirent_lba` and `*out_dirent_off` with the
 * on-disk position of the matched directory entry (pass NULL to skip).
 */
static int lookup_in_dir(FatVolume *vol, uint32_t dir_cluster,
                         uint32_t root_lba, uint32_t root_sectors,
                         const char *component,
                         uint32_t *out_cluster, uint32_t *out_size,
                         int *out_is_dir,
                         uint32_t *out_dirent_lba,
                         uint32_t *out_dirent_off) {
    /* Use the sector-level scanner so we always know the exact disk position */
    SecIter it;
    if (dir_cluster == 0 && root_sectors > 0) {
        /* FAT12/16 root: set up root-dir mode manually */
        memset(&it, 0, sizeof(it));
        it.vol          = vol;
        it.is_root      = 1;
        it.root_lba     = root_lba;
        it.root_sectors = root_sectors;
    } else {
        seciter_init(&it, vol, dir_cluster);
    }

    uint32_t  bps = vol->bytes_per_sector;
    uint32_t  epb = bps / 32;

    FatLfnEntry lfn_buf[MAX_LFN_ENTRIES];
    int         lfn_cnt = 0;

    int r;
    while ((r = seciter_next(&it)) == 1) {
        for (uint32_t i = 0; i < epb; i++) {
            uint32_t  off = i * 32;
            uint8_t  *e   = it.sector + off;

            if (e[0] == FAT_DIRENT_END) { errno = ENOENT; return -1; }
            if (e[0] == FAT_DIRENT_FREE) { lfn_cnt = 0; continue; }

            if ((e[11] & 0x3F) == FAT_ATTR_LONG_NAME) {
                if (lfn_cnt < MAX_LFN_ENTRIES)
                    memcpy(&lfn_buf[lfn_cnt++], e, 32);
                continue;
            }
            if (e[11] & FAT_ATTR_VOLUME_ID) { lfn_cnt = 0; continue; }

            /* Compare short name */
            char sname[13];
            decode_short_name(e, sname);
            int match = (strcasecmp(sname, component) == 0);

            /* Also compare assembled LFN if present */
            if (!match && lfn_cnt > 0) {
                char lname[256];
                assemble_lfn(lfn_buf, lfn_cnt, lname, sizeof(lname));
                match = (strcasecmp(lname, component) == 0);
            }

            if (match) {
                const FatDirEntry *de = (const FatDirEntry *)e;
                *out_cluster = ((uint32_t)de->first_cluster_hi << 16) |
                               de->first_cluster_lo;
                *out_size    = de->file_size;
                *out_is_dir  = (de->attr & FAT_ATTR_DIRECTORY) != 0;
                if (out_dirent_lba) *out_dirent_lba = it.cur_lba;
                if (out_dirent_off) *out_dirent_off = off;
                return 0;
            }
            lfn_cnt = 0;
        }
    }
    errno = ENOENT;
    return -1;
}

static int find_entry(FatVolume *vol, const char *path,
                      uint32_t *first_cluster, uint32_t *file_size,
                      int *is_dir) {
    return find_entry_ex(vol, path, first_cluster, file_size, is_dir,
                         NULL, NULL);
}

static int find_entry_ex(FatVolume *vol, const char *path,
                         uint32_t *first_cluster, uint32_t *file_size,
                         int *is_dir,
                         uint32_t *dirent_lba_out,
                         uint32_t *dirent_off_out) {
    /* Normalise path: strip leading / or drive letter */
    const char *p = path;
    if (p[0] && p[1] == ':') p += 2;
    if (*p == '/' || *p == '\\') p++;

    uint32_t cur_cluster = (vol->type == FAT_TYPE_FAT32)
                           ? vol->root_cluster : 0;
    int      cur_is_dir  = 1;
    uint32_t cur_size    = 0;
    uint32_t cur_dlba    = 0;
    uint32_t cur_doff    = 0;

    if (*p == '\0') {
        *first_cluster = cur_cluster;
        *file_size     = 0;
        *is_dir        = 1;
        if (dirent_lba_out) *dirent_lba_out = 0;
        if (dirent_off_out) *dirent_off_out = 0;
        return 0;
    }

    char component[256];
    while (*p) {
        size_t i = 0;
        while (*p && *p != '/' && *p != '\\' && i < sizeof(component) - 1)
            component[i++] = *p++;
        component[i] = '\0';
        if (*p) p++;
        if (!component[0]) continue;

        if (!cur_is_dir) { errno = ENOTDIR; return -1; }

        if (lookup_in_dir(vol, cur_cluster,
                          vol->root_dir_lba, vol->root_dir_sectors,
                          component,
                          &cur_cluster, &cur_size, &cur_is_dir,
                          &cur_dlba, &cur_doff) != 0)
            return -1;
    }

    *first_cluster = cur_cluster;
    *file_size     = cur_size;
    *is_dir        = cur_is_dir;
    if (dirent_lba_out) *dirent_lba_out = cur_dlba;
    if (dirent_off_out) *dirent_off_out = cur_doff;
    return 0;
}

/* ── Public API — write / create / unlink / flush ───────────────────── */

/*
 * fat_write — write `len` bytes to an open FatFile starting at file->pos.
 *
 * The file must have been obtained via fat_open() after fat_create() so that
 * dirent_lba and dirent_off are populated.  Writing sequentially from pos=0
 * is the primary supported mode; the cluster chain is grown as needed.
 * Call fat_flush() after writing to ensure the on-disk file_size is updated.
 */
ssize_t fat_write(FatFile *file, const void *buf, size_t len) {
    if (!file || !buf || len == 0) { errno = EINVAL; return -1; }
    if (file->dirent_lba == 0)     { errno = EBADF;  return -1; }

    FatVolume  *vol  = file->vol;
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        /* ── Allocate first cluster for a brand-new empty file ── */
        if (file->first_cluster == 0) {
            uint32_t c = fat_alloc_cluster(vol, 0);
            if (c == 0) break; /* ENOSPC set by fat_alloc_cluster */
            file->first_cluster  = c;
            file->cur_cluster    = c;
            file->last_cluster   = c;
            file->cur_cluster_idx = 0;
            /* Load a zeroed buffer for the new cluster */
            if (!file->cluster_buf) {
                file->cluster_buf = calloc(1, vol->bytes_per_cluster);
                if (!file->cluster_buf) { errno = ENOMEM; break; }
            } else {
                memset(file->cluster_buf, 0, vol->bytes_per_cluster);
            }
            file->buf_dirty = 0;
        }

        /* Ensure cluster buffer is loaded */
        if (!file->cluster_buf) {
            if (file_load_cluster(file, file->cur_cluster) != 0) break;
        }

        uint32_t cluster_offset = file->pos % vol->bytes_per_cluster;
        uint32_t space = vol->bytes_per_cluster - cluster_offset;
        size_t   to_write = (len - done < (size_t)space)
                            ? (len - done) : (size_t)space;

        memcpy(file->cluster_buf + cluster_offset, src + done, to_write);
        file->buf_dirty = 1;
        file->pos      += (uint32_t)to_write;
        done           += to_write;

        /* If we filled this cluster and there is more to write, flush and
           allocate / advance to the next cluster. */
        if (file->pos % vol->bytes_per_cluster == 0 && done < len) {
            /* Flush current cluster */
            if (write_cluster(vol, file->cur_cluster,
                              file->cluster_buf) != 0) break;
            file->buf_dirty = 0;

            /* Extend chain or follow existing link */
            uint32_t next = fat_next_cluster(vol, file->cur_cluster);
            if (next >= FAT32_CLUSTER_EOC) {
                next = fat_alloc_cluster(vol, file->cur_cluster);
                if (next == 0) break;
                file->last_cluster = next;
            }
            file->cur_cluster = next;
            file->cur_cluster_idx++;
            /* Zero out buffer for the new cluster */
            memset(file->cluster_buf, 0, vol->bytes_per_cluster);
        }
    }

    /* Update cached file_size */
    if (file->pos > file->file_size)
        file->file_size = file->pos;

    return (ssize_t)done;
}

/*
 * fat_flush — write the dirty cluster buffer to disk and update the
 * on-disk directory entry's file_size field.
 */
int fat_flush(FatFile *file) {
    if (!file) return 0;
    FatVolume *vol = file->vol;

    /* Write dirty cluster buffer */
    if (file->buf_dirty && file->cur_cluster >= 2) {
        if (write_cluster(vol, file->cur_cluster, file->cluster_buf) != 0)
            return -1;
        file->buf_dirty = 0;
    }

    /* Update file_size in the directory entry */
    if (file->dirent_lba != 0) {
        uint8_t  sector[4096];
        uint64_t off = lba_to_offset(vol, file->dirent_lba);
        if (read_at(vol->fd, sector, vol->bytes_per_sector, off) != 0)
            return -1;

        FatDirEntry *de = (FatDirEntry *)(sector + file->dirent_off);
        de->file_size        = file->file_size;
        /* Update first cluster pointers for files that had no cluster before */
        if (file->first_cluster >= 2) {
            de->first_cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
            de->first_cluster_hi = (uint16_t)((file->first_cluster >> 16) & 0xFFFF);
        }
        if (write_at(vol->fd, sector, vol->bytes_per_sector, off) != 0)
            return -1;
    }

    /* Persist FSInfo */
    fat_update_fsinfo(vol);
    return 0;
}

/*
 * fat_create — create a new file or directory at `path`.
 *
 * Only 8.3 filenames are supported.  Parent directory must already exist.
 * On success the file is empty (no data clusters allocated yet); use
 * fat_open() + fat_write() + fat_flush() to populate it.
 */
int fat_create(FatVolume *vol, const char *path, int is_dir) {
    if (vol->type != FAT_TYPE_FAT32 && vol->type != FAT_TYPE_FAT16) {
        errno = ENOSYS;
        return -1;
    }

    /* Split into parent cluster + basename */
    uint32_t parent_cluster = 0;
    char     basename[256];
    if (split_path(vol, path, &parent_cluster, basename, sizeof(basename)) != 0)
        return -1;
    if (!basename[0]) { errno = EINVAL; return -1; }

    /* Convert to FAT 8.3 name */
    uint8_t fat_name[11];
    if (to_fat_name(basename, fat_name) != 0) { errno = EINVAL; return -1; }

    /* Check it doesn't already exist */
    {
        uint32_t ex_lba = 0, ex_off = 0;
        if (dirscan_find(vol, parent_cluster, basename, 0,
                         &ex_lba, &ex_off,
                         NULL, NULL, NULL, NULL) == 1) {
            errno = EEXIST;
            return -1;
        }
    }

    /* Find a free slot in the parent directory */
    uint32_t slot_lba = 0, slot_off = 0;
    if (dirscan_find(vol, parent_cluster, NULL, 1,
                     &slot_lba, &slot_off,
                     NULL, NULL, NULL, NULL) != 1) {
        /* TODO: grow directory cluster chain when full */
        errno = ENOSPC;
        return -1;
    }

    /* Build the directory entry */
    FatDirEntry de;
    memset(&de, 0, sizeof(de));
    memcpy(de.name, fat_name, 11);
    de.attr = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;

    /* Timestamp: use current time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        uint16_t fat_date = (uint16_t)(((tm->tm_year - 80) << 9) |
                                        ((tm->tm_mon + 1)   << 5) |
                                         tm->tm_mday);
        uint16_t fat_time = (uint16_t)((tm->tm_hour << 11) |
                                        (tm->tm_min  <<  5) |
                                        (tm->tm_sec / 2));
        de.write_date  = fat_date;
        de.write_time  = fat_time;
        de.create_date = fat_date;
        de.create_time = fat_time;
    }

    /* For directories, allocate a cluster and add . and .. entries */
    if (is_dir) {
        uint32_t dir_cluster = fat_alloc_cluster(vol, 0);
        if (dir_cluster == 0) return -1;

        de.first_cluster_lo = (uint16_t)(dir_cluster & 0xFFFF);
        de.first_cluster_hi = (uint16_t)((dir_cluster >> 16) & 0xFFFF);

        /* Zero the new cluster */
        uint8_t *zero = calloc(1, vol->bytes_per_cluster);
        if (!zero) { fat_free_cluster_chain(vol, dir_cluster); return -1; }
        if (write_cluster(vol, dir_cluster, zero) != 0) {
            free(zero);
            fat_free_cluster_chain(vol, dir_cluster);
            return -1;
        }

        /* Write . and .. entries */
        FatDirEntry dot;
        memset(&dot, 0, sizeof(dot));
        memset(dot.name, ' ', 11);
        dot.name[0] = '.';
        dot.attr = FAT_ATTR_DIRECTORY;
        dot.first_cluster_lo = de.first_cluster_lo;
        dot.first_cluster_hi = de.first_cluster_hi;
        dot.write_date = de.write_date;
        dot.write_time = de.write_time;

        FatDirEntry dotdot = dot;
        dotdot.name[1] = '.';
        dotdot.first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);
        dotdot.first_cluster_hi = (uint16_t)((parent_cluster >> 16) & 0xFFFF);

        uint32_t dir_lba = cluster_to_lba(vol, dir_cluster);
        uint8_t sector[512];
        memset(sector, 0, sizeof(sector));
        memcpy(sector,      &dot,    32);
        memcpy(sector + 32, &dotdot, 32);
        if (write_at(vol->fd, sector, sizeof(sector),
                     lba_to_offset(vol, dir_lba)) != 0) {
            free(zero);
            fat_free_cluster_chain(vol, dir_cluster);
            return -1;
        }
        free(zero);
    }

    /* Write the new entry into the parent directory slot */
    uint8_t  pbuf[4096];
    uint64_t poff = lba_to_offset(vol, slot_lba);
    if (read_at(vol->fd, pbuf, vol->bytes_per_sector, poff) != 0) return -1;
    memcpy(pbuf + slot_off, &de, 32);
    /* If this was an end-of-directory slot, write a new end marker after it */
    if (slot_off + 32 < vol->bytes_per_sector)
        pbuf[slot_off + 32] = FAT_DIRENT_END;
    if (write_at(vol->fd, pbuf, vol->bytes_per_sector, poff) != 0) return -1;

    fat_update_fsinfo(vol);
    return 0;
}

/*
 * fat_unlink — delete a file or empty directory.
 *
 * Marks the directory entry (and any preceding LFN entries) as free (0xE5)
 * and releases the cluster chain.
 */
int fat_unlink(FatVolume *vol, const char *path) {
    if (vol->type != FAT_TYPE_FAT32 && vol->type != FAT_TYPE_FAT16) {
        errno = ENOSYS;
        return -1;
    }

    /* Find the parent directory cluster and basename */
    uint32_t parent_cluster = 0;
    char     basename[256];
    if (split_path(vol, path, &parent_cluster, basename, sizeof(basename)) != 0)
        return -1;

    /* Find the entry and its LFN companions */
    uint32_t match_lba = 0, match_off = 0;
    uint8_t  match_raw[32];
    uint32_t lfn_lba[DIRSCAN_MAX_LFN];
    uint32_t lfn_off[DIRSCAN_MAX_LFN];
    int      lfn_cnt = 0;

    if (dirscan_find(vol, parent_cluster, basename, 0,
                     &match_lba, &match_off, match_raw,
                     lfn_lba, lfn_off, &lfn_cnt) != 1) {
        errno = ENOENT;
        return -1;
    }

    const FatDirEntry *de = (const FatDirEntry *)match_raw;

    /* Refuse to unlink a non-empty directory */
    if (de->attr & FAT_ATTR_DIRECTORY) {
        uint32_t dir_cluster = ((uint32_t)de->first_cluster_hi << 16) |
                                de->first_cluster_lo;
        if (dir_cluster >= 2) {
            /* Quick non-empty check: look for any non-dot entry */
            SecIter it;
            seciter_init(&it, vol, dir_cluster);
            int ir;
            while ((ir = seciter_next(&it)) == 1) {
                for (uint32_t i = 0; i < vol->bytes_per_sector / 32; i++) {
                    uint8_t *e = it.sector + i * 32;
                    if (e[0] == FAT_DIRENT_END) goto empty_dir;
                    if (e[0] == FAT_DIRENT_FREE) continue;
                    if ((e[11] & 0x3F) == FAT_ATTR_LONG_NAME) continue;
                    if (e[11] & FAT_ATTR_VOLUME_ID) continue;
                    /* Real entry: check for . and .. */
                    char sn[13]; decode_short_name(e, sn);
                    if (strcmp(sn, ".") == 0 || strcmp(sn, "..") == 0) continue;
                    errno = ENOTEMPTY; return -1;
                }
            }
        }
    }
empty_dir:;

    /* Mark LFN entries as free */
    for (int k = 0; k < lfn_cnt; k++) {
        uint8_t  sector[4096];
        uint64_t off = lba_to_offset(vol, lfn_lba[k]);
        if (read_at(vol->fd, sector, vol->bytes_per_sector, off) == 0) {
            sector[lfn_off[k]] = FAT_DIRENT_FREE;
            write_at(vol->fd, sector, vol->bytes_per_sector, off);
        }
    }

    /* Mark 8.3 entry as free */
    {
        uint8_t  sector[4096];
        uint64_t off = lba_to_offset(vol, match_lba);
        if (read_at(vol->fd, sector, vol->bytes_per_sector, off) != 0)
            return -1;
        sector[match_off] = FAT_DIRENT_FREE;
        if (write_at(vol->fd, sector, vol->bytes_per_sector, off) != 0)
            return -1;
    }

    /* Free the cluster chain */
    uint32_t first_cluster = ((uint32_t)de->first_cluster_hi << 16) |
                              de->first_cluster_lo;
    if (first_cluster >= 2)
        fat_free_cluster_chain(vol, first_cluster);

    fat_update_fsinfo(vol);
    return 0;
}

/*
 * fat_check_free_count — count free clusters by scanning the FAT table and
 * compare with the FSInfo cached value.
 */
int fat_check_free_count(FatVolume *vol, uint32_t *free_count_out,
                         uint32_t *fsinfo_count_out) {
    if (!vol || !free_count_out) { errno = EINVAL; return -1; }

    uint32_t free_count = 0;
    uint8_t  buf[512];
    uint32_t prev_sector = (uint32_t)-1;

    for (uint32_t c = 2; c < vol->total_clusters + 2; c++) {
        uint32_t fat_offset, sector, entry_offset;
        uint32_t val = 1; /* assume non-free */

        if (vol->type == FAT_TYPE_FAT32) {
            fat_offset   = c * 4;
            sector       = vol->fat_start_lba + fat_offset / vol->bytes_per_sector;
            entry_offset = fat_offset % vol->bytes_per_sector;
        } else if (vol->type == FAT_TYPE_FAT16) {
            fat_offset   = c * 2;
            sector       = vol->fat_start_lba + fat_offset / vol->bytes_per_sector;
            entry_offset = fat_offset % vol->bytes_per_sector;
        } else {
            break; /* FAT12 not supported */
        }

        if (sector != prev_sector) {
            if (read_at(vol->fd, buf, 512,
                        lba_to_offset(vol, sector)) != 0) return -1;
            prev_sector = sector;
        }

        if (vol->type == FAT_TYPE_FAT32) {
            val = ((uint32_t)buf[entry_offset]       |
                   ((uint32_t)buf[entry_offset+1] <<  8) |
                   ((uint32_t)buf[entry_offset+2] << 16) |
                   ((uint32_t)buf[entry_offset+3] << 24)) & 0x0FFFFFFFU;
        } else {
            uint16_t v16 = (uint16_t)(buf[entry_offset] |
                                      ((uint16_t)buf[entry_offset+1] << 8));
            val = v16;
        }

        if (val == FAT32_CLUSTER_FREE) free_count++;
    }

    *free_count_out = free_count;
    if (fsinfo_count_out)
        *fsinfo_count_out = vol->free_cluster_count;
    return 0;
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
