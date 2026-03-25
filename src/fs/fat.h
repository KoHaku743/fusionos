#ifndef FUSION_FAT_H
#define FUSION_FAT_H

/*
 * FusionOS FAT32 filesystem driver
 *
 * Supports reading (and writing) FAT12 / FAT16 / FAT32 / exFAT volumes
 * from raw block devices or image files.  Long Filename (LFN / VFAT)
 * support is included.
 *
 * FAT32 is the native filesystem for the FusionOS boot partition and the
 * primary install target for DOS-compatible storage.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>  /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Filesystem type ─────────────────────────────────────────────────── */
typedef enum {
    FAT_TYPE_UNKNOWN = 0,
    FAT_TYPE_FAT12,
    FAT_TYPE_FAT16,
    FAT_TYPE_FAT32,
    FAT_TYPE_EXFAT,
} FatType;

/* ── On-disk BPB (BIOS Parameter Block) structures ──────────────────── */

/**
 * Common DOS 3.31 BPB header (first 36 bytes after the jump instruction).
 * Present in FAT12, FAT16, and FAT32.
 */
typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];       /* 0x00 – Boot jump instruction       */
    uint8_t  oem_name[8];       /* 0x03 – OEM identifier               */
    uint16_t bytes_per_sector;  /* 0x0B – Must be 512, 1024, 2048, 4096 */
    uint8_t  sectors_per_cluster; /* 0x0D – Power of 2; 1–128          */
    uint16_t reserved_sectors;  /* 0x0E – Number of reserved sectors   */
    uint8_t  num_fats;          /* 0x10 – Number of FAT copies (2)     */
    uint16_t root_entry_count;  /* 0x11 – 0 for FAT32                  */
    uint16_t total_sectors_16;  /* 0x13 – Total sectors (0 if ≥65536)  */
    uint8_t  media_type;        /* 0x15 – 0xF8 = fixed disk            */
    uint16_t fat_size_16;       /* 0x16 – Sectors per FAT; 0 for FAT32 */
    uint16_t sectors_per_track; /* 0x18 – For interrupt 0x13           */
    uint16_t num_heads;         /* 0x1A – For interrupt 0x13           */
    uint32_t hidden_sectors;    /* 0x1C – LBA of partition start       */
    uint32_t total_sectors_32;  /* 0x20 – Total sectors (if ≥65536)    */
} FatBpbCommon;

/** FAT32-specific extended BPB (bytes 36–89). */
typedef struct __attribute__((packed)) {
    uint32_t fat_size_32;       /* 0x24 – Sectors per FAT              */
    uint16_t ext_flags;         /* 0x28 – FAT mirroring flags          */
    uint16_t fs_version;        /* 0x2A – Must be 0x0000               */
    uint32_t root_cluster;      /* 0x2C – First cluster of root dir    */
    uint16_t fs_info;           /* 0x30 – Sector of FSInfo structure   */
    uint16_t backup_boot_sector;/* 0x32 – Backup boot sector           */
    uint8_t  reserved[12];      /* 0x34 – Reserved                     */
    uint8_t  drive_number;      /* 0x40 – 0x80 for fixed disk          */
    uint8_t  reserved1;
    uint8_t  boot_signature;    /* 0x42 – 0x29 if next 3 fields valid  */
    uint32_t volume_id;         /* 0x43 – Volume serial number         */
    uint8_t  volume_label[11];  /* 0x47 – Volume label (space-padded)  */
    uint8_t  fs_type[8];        /* 0x52 – "FAT32   "                   */
} Fat32Ebpb;

/** FAT12/FAT16 extended BPB (bytes 36–61). */
typedef struct __attribute__((packed)) {
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;    /* 0x29 if next 3 fields valid         */
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];        /* "FAT12   " or "FAT16   "            */
} Fat16Ebpb;

/* ── Directory entries ───────────────────────────────────────────────── */

/** Standard 8.3 directory entry (32 bytes). */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];          /* 8.3 name (space-padded, uppercase)  */
    uint8_t  attr;              /* File attributes                     */
    uint8_t  nt_reserved;       /* Reserved for Windows NT             */
    uint8_t  create_time_tenth; /* Tenths of second at creation        */
    uint16_t create_time;       /* Creation time                       */
    uint16_t create_date;       /* Creation date                       */
    uint16_t last_access_date;  /* Last access date                    */
    uint16_t first_cluster_hi;  /* High 16 bits of first cluster (FAT32) */
    uint16_t write_time;        /* Time of last write                  */
    uint16_t write_date;        /* Date of last write                  */
    uint16_t first_cluster_lo;  /* Low 16 bits of first cluster        */
    uint32_t file_size;         /* File size in bytes (0 for dirs)     */
} FatDirEntry;

/** LFN (Long Filename) directory entry (32 bytes). */
typedef struct __attribute__((packed)) {
    uint8_t  order;             /* Sequence; 0x40 | n for last entry   */
    uint16_t name1[5];          /* UTF-16 characters 1–5               */
    uint8_t  attr;              /* Must be ATTR_LONG_NAME (0x0F)       */
    uint8_t  type;              /* Must be 0                           */
    uint8_t  checksum;          /* Checksum of 8.3 name                */
    uint16_t name2[6];          /* UTF-16 characters 6–11              */
    uint16_t first_cluster_lo;  /* Must be 0                           */
    uint16_t name3[2];          /* UTF-16 characters 12–13             */
} FatLfnEntry;

/* File attribute bits */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | \
                             FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* Special first-byte values in dir-entry name[0] */
#define FAT_DIRENT_FREE     0xE5  /* Entry is free / was deleted */
#define FAT_DIRENT_END      0x00  /* No more entries in directory */

/* FAT32 special cluster values */
#define FAT32_CLUSTER_FREE  0x00000000U
#define FAT32_CLUSTER_BAD   0x0FFFFFF7U
#define FAT32_CLUSTER_EOC   0x0FFFFFF8U  /* end-of-chain (≥ this value) */

/* ── FSInfo structure (FAT32 only) ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t lead_sig;          /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_sig;        /* 0x61417272 */
    uint32_t free_count;        /* Last known free cluster count       */
    uint32_t next_free;         /* Hint for next free cluster          */
    uint8_t  reserved2[12];
    uint32_t trail_sig;         /* 0xAA550000 */
} FatFsInfo;

/* ── In-memory volume descriptor ─────────────────────────────────────── */
typedef struct {
    int       fd;               /* File descriptor (block device / image) */
    FatType   type;
    uint64_t  partition_lba;    /* LBA of partition start (0 if whole disk) */

    /* Derived from BPB */
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  reserved_sectors;
    uint32_t  num_fats;
    uint32_t  fat_start_lba;    /* LBA of first FAT */
    uint32_t  fat_sectors;      /* Sectors per FAT  */
    uint32_t  data_start_lba;   /* LBA of first data cluster */
    uint32_t  root_cluster;     /* First cluster of root dir (FAT32) */
    uint32_t  root_dir_lba;     /* LBA of root dir (FAT12/16) */
    uint32_t  root_dir_sectors; /* Sector count of root dir (FAT12/16) */
    uint32_t  total_clusters;

    /* Volume label */
    char      volume_label[12]; /* NUL-terminated */

    /* FSInfo (FAT32 only) */
    uint16_t  fs_info_sector;     /* Sector number of FSInfo struct; 0 = none */
    uint32_t  free_cluster_count; /* Cached free cluster count; 0xFFFFFFFF = unknown */
    uint32_t  next_free_cluster;  /* Allocation hint; 0xFFFFFFFF = unknown */
} FatVolume;

/* ── File handle ─────────────────────────────────────────────────────── */
typedef struct {
    FatVolume *vol;
    uint32_t   first_cluster;
    uint32_t   file_size;        /* 0 for directories */
    int        is_dir;

    /* Current read/write position */
    uint32_t   pos;              /* Byte offset from start */
    uint32_t   cur_cluster;      /* Cluster currently buffered */
    uint32_t   cur_cluster_idx;  /* Which cluster in the chain (0-based) */
    uint8_t   *cluster_buf;      /* Allocated cluster-sized buffer */

    /* Write support */
    uint32_t   dirent_lba;       /* LBA of sector holding this file's dir entry */
    uint32_t   dirent_off;       /* Byte offset within dirent_lba (0 = unknown) */
    uint32_t   last_cluster;     /* Last cluster in chain; 0 = not yet resolved */
    int        buf_dirty;        /* 1 = cluster_buf modified and needs flush */
} FatFile;

/* ── Directory iterator ───────────────────────────────────────────────── */
typedef struct {
    FatFile  *dir_file;
    uint32_t  entry_offset;     /* Byte offset within directory stream */
} FatDir;

/* ── Directory entry (user-visible) ─────────────────────────────────── */
typedef struct {
    char     name[256];         /* Long filename (or 8.3 if no LFN)   */
    char     short_name[13];    /* 8.3 name, NUL-terminated            */
    int      is_dir;
    int      is_hidden;
    int      is_read_only;
    uint32_t file_size;
    uint32_t first_cluster;
    /* Timestamps (seconds since Unix epoch) */
    int64_t  create_time;
    int64_t  modify_time;
    int64_t  access_time;
} FatDirEnt;

/* ── Public API ───────────────────────────────────────────────────────── */

/**
 * Mount a FAT volume.
 *
 * @param path        Path to block device or image file.
 * @param partition   Partition index (0 = whole disk / no MBR, 1-4 = MBR).
 * @param[out] vol    Output volume descriptor.
 * @return  0 on success, -1 on error (errno set).
 */
int fat_mount(const char *path, int partition, FatVolume *vol);

/** Unmount and release resources. */
void fat_unmount(FatVolume *vol);

/**
 * Open a file or directory by absolute path (DOS or POSIX separators).
 *
 * @return 0 on success, -1 on error.
 */
int fat_open(FatVolume *vol, const char *path, FatFile *out);

/** Close a FatFile. */
void fat_close(FatFile *file);

/**
 * Read bytes from an open file.
 *
 * @return Bytes read, 0 on EOF, -1 on error.
 */
ssize_t fat_read(FatFile *file, void *buf, size_t len);

/** Seek within a file (SEEK_SET only). */
int fat_seek(FatFile *file, uint32_t offset);

/**
 * Open a directory for iteration.
 *
 * @return 0 on success, -1 if not a directory.
 */
int fat_opendir(FatFile *dir_file, FatDir *out);

/**
 * Read the next directory entry.
 *
 * @return 1 if entry read, 0 at end, -1 on error.
 */
int fat_readdir(FatDir *dir, FatDirEnt *ent);

/** Close a directory iterator. */
void fat_closedir(FatDir *dir);

/**
 * Create a new file.  Parent directory must exist.
 *
 * @return 0 on success, -1 on error.
 */
int fat_create(FatVolume *vol, const char *path, int is_dir);

/**
 * Write bytes to an open file (must have been opened via fat_create
 * or a write-capable path in future).
 *
 * @return Bytes written, or -1 on error.
 */
ssize_t fat_write(FatFile *file, const void *buf, size_t len);

/**
 * Delete a file or empty directory.
 *
 * @return 0 on success, -1 on error.
 */
int fat_unlink(FatVolume *vol, const char *path);

/**
 * Flush any dirty write buffer and update the on-disk directory entry
 * file_size field.  Must be called after fat_write to persist changes.
 *
 * @return 0 on success, -1 on error.
 */
int fat_flush(FatFile *file);

/**
 * Count free clusters and validate the FSInfo free-cluster count.
 *
 * @param[out] free_count   Number of free clusters found by FAT scan.
 * @param[out] fsinfo_count FSInfo free_count field (0xFFFFFFFF if N/A).
 * @return 0 on success, -1 on error.
 */
int fat_check_free_count(FatVolume *vol, uint32_t *free_count,
                         uint32_t *fsinfo_count);

/**
 * Return a human-readable filesystem type string.
 */
const char *fat_type_name(FatType type);

/**
 * Convert FAT date/time fields to a Unix timestamp.
 */
int64_t fat_datetime_to_unix(uint16_t date, uint16_t time_field,
                              uint8_t tenths);

#ifdef __cplusplus
}
#endif

#endif /* FUSION_FAT_H */
