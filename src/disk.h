#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DISK_MAX_TRACKS   84
#define DISK_MAX_SIDES     2
#define DISK_MAX_SECTORS  29
#define DISK_PATH_MAX    4096

typedef struct {
    uint8_t C, H, R, N;    /* CHRN — cylinder, head, record, size code */
    uint8_t st1, st2;       /* pre-set status flags from DSK file */
    int     offset;         /* byte offset within track data[] */
    long    file_offset;    /* absolute byte offset of sector data in .dsk */
    int     size;           /* actual data bytes (128 << N, or per extended header) */
} DiskSector;

typedef struct {
    int        sector_count;
    DiskSector sectors[DISK_MAX_SECTORS];
    uint8_t   *data;        /* raw sector data (heap-allocated) */
    int        data_size;
} DiskTrack;

typedef struct {
    bool      inserted;
    bool      write_protected;
    char      path[DISK_PATH_MAX];
    int       track_count;
    int       sides;
    DiskTrack track[DISK_MAX_TRACKS][DISK_MAX_SIDES];
    int       cur_track;    /* current head position */
    int       cur_sector;   /* last-used sector index (for READ ID rotation) */
} Disk;

void disk_init(Disk *d);
void disk_eject(Disk *d);

/* Returns 0 on success, -1 on error. */
int  disk_load(Disk *d, const char *path);

/* Write a standard CPC DATA-format blank .dsk to `path` (40 tracks,
 * single-sided, 9 × 512-byte sectors, sector IDs 0xC1..0xC9, 0xE5 fill
 * — empty AMSDOS directory). Returns 0 on success, -1 on I/O error. */
int  disk_create_blank(const char *path);

/* Find a sector by CHRN on the current track. Returns NULL if not found. */
DiskSector *disk_find_sector(Disk *d, int side, uint8_t C, uint8_t H,
                             uint8_t R, uint8_t N);

/* Persist an already-committed sector write back into the mounted .dsk image. */
int disk_write_sector(Disk *d, DiskSector *sec, const uint8_t *data, int len);
