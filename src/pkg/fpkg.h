/*
 * fpkg.h — FusionOS Package (.fpkg) format v1 definition
 *
 * On-disk layout (all multi-byte integers are little-endian):
 *
 *   [FpkgHeader        ]   520 bytes
 *   [FpkgFileEntry × N ]   FPKG_ENTRY_SIZE × file_count bytes
 *   [Pre-install script]   header.pre_script_size bytes  (BAT text)
 *   [Post-install script]  header.post_script_size bytes (BAT text)
 *   [Payload           ]   header.payload_size bytes     (concatenated files)
 *   [SHA-256 trailer   ]   32 bytes (SHA-256 of all preceding bytes)
 *
 * Verification: compute SHA-256 from byte 0 to (file_size - 32), compare
 * against the final 32 bytes of the file.
 *
 * Install-destination mapping (DOS path → POSIX path):
 *   C:\BIN\*   → /usr/local/bin/
 *   C:\DOS\*   → /usr/local/dos/
 *   C:\GAMES\* → /usr/local/games/
 *   C:\*       → /opt/fusionos/
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Magic & version ────────────────────────────────────────────────── */

#define FPKG_MAGIC          "FPKG"
#define FPKG_MAGIC_SIZE     4
#define FPKG_FORMAT_VERSION 1

/* ── Flags (header.flags) ───────────────────────────────────────────── */
#define FPKG_FLAG_NONE      0x0000

/* ── Field sizes ────────────────────────────────────────────────────── */
#define FPKG_NAME_MAX       128
#define FPKG_VER_MAX        64
#define FPKG_ARCH_MAX       32
#define FPKG_DESC_MAX       256
#define FPKG_PATH_MAX       512

/* ── SHA-256 trailer size ───────────────────────────────────────────── */
#define FPKG_SHA256_SIZE    32

/* ── Default install prefixes ───────────────────────────────────────── */
#define FPKG_PREFIX_ROOT    "/opt/fusionos"
#define FPKG_PREFIX_BIN     "/usr/local/bin"
#define FPKG_PREFIX_DOS     "/usr/local/dos"
#define FPKG_PREFIX_GAMES   "/usr/local/games"

/* ── FpkgHeader (520 bytes) ─────────────────────────────────────────── */
/*
 * All fields NUL-padded.  No internal pointers — everything is
 * self-contained via absolute/relative offsets.
 */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];           /* "FPKG" — no NUL terminator          */
    uint16_t fmt_version;        /* FPKG_FORMAT_VERSION                 */
    uint16_t flags;              /* FPKG_FLAG_* bitmask                 */
    char     name[128];          /* Package name, NUL-padded            */
    char     version[64];        /* e.g. "1.0.0", NUL-padded           */
    char     arch[32];           /* "x86_64", "any", NUL-padded         */
    char     description[256];   /* Human-readable description          */
    uint32_t file_count;         /* Number of FpkgFileEntry records     */
    uint32_t pre_script_size;    /* Size of pre-install BAT (0 = none)  */
    uint32_t post_script_size;   /* Size of post-install BAT (0 = none) */
    uint32_t reserved;           /* Must be 0                           */
    uint64_t payload_offset;     /* Absolute byte offset to payload     */
    uint64_t payload_size;       /* Total payload bytes                 */
} FpkgHeader;   /* 4+2+2+128+64+32+256+4+4+4+4+8+8 = 520 bytes */
#pragma pack(pop)

/* Compile-time size guard — verified in fpkg_create and fusion_pkg */
#define FPKG_HEADER_SIZE    520

/* ── FpkgFileEntry (536 bytes) ─────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    char     path[512];     /* DOS path, e.g. "C:\\BIN\\TOOL.EXE"      */
    uint64_t size;          /* File size in bytes                       */
    uint64_t payload_off;   /* Offset into the payload section          */
    uint32_t mode;          /* Unix permissions (e.g. 0755)             */
    uint32_t reserved;      /* Must be 0                                */
} FpkgFileEntry;  /* 512+8+8+4+4 = 536 bytes */
#pragma pack(pop)

#define FPKG_ENTRY_SIZE     536
