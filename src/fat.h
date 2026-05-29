#pragma once
#include "types.h"
#include <stdbool.h>
#include <stdio.h>

/* Minimal FAT16/FAT32 driver, used by the M4 emulation when the SD card is
 * backed by a raw image file with no host directory companion. Supports the
 * file operations BASIC and friendly tools issue through the M4 command set:
 * directory listing, change-directory, open/read/write/seek/close, file size
 * and free-space queries. Long filenames are accepted on lookup but the
 * driver only emits short (8.3) names. */

typedef struct FatVol {
    FILE   *fp;

    /* BPB-derived layout */
    u16     bytes_per_sector;
    u8      sectors_per_cluster;
    u16     reserved_sectors;
    u8      num_fats;
    u16     root_entries;          /* FAT12/16 only (FAT32 = 0) */
    u32     total_sectors;
    u32     fat_size_sectors;
    u32     root_cluster;          /* FAT32 only */

    /* Computed positions, in sectors from the start of the image */
    u32     part_start_lba;        /* MBR's partition 1 start, or 0 if no MBR */
    u32     fat_start_sector;
    u32     root_dir_sector;       /* FAT12/16 fixed root dir start */
    u32     data_start_sector;
    u32     total_clusters;
    int     fat_type;              /* 12, 16, or 32 (12 unused but detected) */
    u32     bytes_per_cluster;

    /* Single-sector FAT cache */
    u32     fat_cache_sector;      /* (u32)-1 = invalid */
    u8      fat_cache[512];
    bool    fat_dirty;
} FatVol;

typedef struct FatDir {
    FatVol *vol;
    /* For FAT12/16 root dir: cluster=0 and we walk root_dir_sector..root_end.
     * Otherwise we follow the cluster chain starting at first_cluster. */
    u32     first_cluster;
    u32     cur_cluster;
    u32     cur_sector_in_cluster; /* 0..spc-1 */
    u32     cur_root_sector;       /* FAT12/16 root walk */
    u32     entry_in_sector;       /* 0..(bps/32-1) */
    bool    at_end;
} FatDir;

typedef struct FatFile {
    FatVol *vol;
    u32     first_cluster;
    u32     file_size;
    u32     file_offset;
    /* Dir-entry location, so writes can update the size+first-cluster fields */
    u32     dir_sector;
    u16     dir_offset;
    bool    write_mode;
    bool    modified;
} FatFile;

/* ---- Volume lifecycle ---- */

/* Mount a FAT volume from an open binary R/W file. Detects MBR + FAT16/32.
 * Returns false if no usable filesystem is found. */
bool fat_mount(FatVol *v, FILE *fp);
void fat_unmount(FatVol *v);

/* Free space in kilobytes. */
u32 fat_free_kb(FatVol *v);

/* ---- Directory ---- */

/* Open the directory at `path`. `path` is `/`-separated, case-insensitive.
 * "/" returns the root directory. Returns NULL if the path is not a dir. */
FatDir *fat_opendir(FatVol *v, const char *path);

/* Read the next entry. Fills `name` (max 13 chars, 8.3 with dot) and `is_dir`.
 * Returns false at end of directory. Skips ".", ".." and deleted/volume
 * entries. */
bool fat_readdir(FatDir *d, char *name, size_t name_sz, u32 *size, bool *is_dir);

void fat_closedir(FatDir *d);

/* True if `path` exists and is a directory. */
bool fat_dir_exists(FatVol *v, const char *path);

/* ---- File ---- */

/* Open a file. `write_create=true` truncates+creates; otherwise read-only. */
FatFile *fat_open(FatVol *v, const char *path, bool write_create);

/* Read up to `size` bytes from the current offset. Returns bytes transferred. */
u32 fat_read(FatFile *f, void *buf, u32 size);

/* Write `size` bytes starting at the current offset. Extends the file as
 * needed (allocates new clusters). Returns bytes written. */
u32 fat_write(FatFile *f, const void *buf, u32 size);

/* Absolute seek. Returns false if `pos` cannot be reached. */
bool fat_seek(FatFile *f, u32 pos);

u32 fat_tell(FatFile *f);
u32 fat_file_size(FatFile *f);

/* Close (flushes the directory entry on writes). */
void fat_close(FatFile *f);

/* Overwrite a 32-byte FAT directory entry at (sector, byte_offset).
 * Used by Albireo's CH376 DIR_INFO_SAVE command, which is how UNIDOS
 * implements renames (it edits the name field of an existing entry).
 * Returns false on I/O error. */
bool fat_write_dir_entry(FatVol *v, u32 sector, u16 byte_offset,
                         const u8 entry[32]);
