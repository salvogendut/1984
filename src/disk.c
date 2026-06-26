#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void disk_init(Disk *d) {
    memset(d, 0, sizeof(*d));
}

void disk_eject(Disk *d) {
    for (int t = 0; t < DISK_MAX_TRACKS; t++)
        for (int s = 0; s < DISK_MAX_SIDES; s++)
            free(d->track[t][s].data);
    int cur_track = d->cur_track;
    memset(d, 0, sizeof(*d));
    d->cur_track = cur_track;   /* preserve head position across eject */
}

static int load_track(DiskTrack *tr, FILE *f, int track_size) {
    if (track_size < 256) return 0;

    uint8_t hdr[256];
    if (fread(hdr, 1, 256, f) != 256) return -1;

    /* "Track-Info" marker */
    if (memcmp(hdr, "Track-Info", 10) != 0) return 0;

    int spt  = hdr[0x15];
    if (spt > DISK_MAX_SECTORS) spt = DISK_MAX_SECTORS;
    tr->sector_count = spt;

    /* Sector data follows the 256-byte header */
    int data_size = track_size - 256;
    if (data_size > 0) {
        tr->data = malloc(data_size);
        if (!tr->data) return -1;
        tr->data_size = (int)fread(tr->data, 1, data_size, f);
    }

    int offset = 0;
    for (int i = 0; i < spt; i++) {
        uint8_t *si = hdr + 0x18 + i * 8;
        DiskSector *sec = &tr->sectors[i];
        sec->C   = si[0];
        sec->H   = si[1];
        sec->R   = si[2];
        sec->N   = si[3];
        sec->st1 = si[4];
        sec->st2 = si[5];
        /* Extended DSK stores actual size in bytes at si[6..7] */
        int sz = (si[7] << 8) | si[6];
        if (sz == 0) sz = 128 << sec->N;
        sec->size   = sz;
        sec->offset = offset;
        offset += sz;
    }
    return 0;
}

int disk_load(Disk *d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "disk: cannot open %s\n", path); return -1; }

    uint8_t hdr[256];
    if (fread(hdr, 1, 256, f) != 256) { fclose(f); return -1; }

    bool extended;
    if      (memcmp(hdr, "MV - CPC", 8) == 0) extended = false;
    else if (memcmp(hdr, "EXTENDED", 8) == 0) extended = true;
    else {
        fprintf(stderr, "disk: %s is not a CPC DSK file\n", path);
        fclose(f);
        return -1;
    }

    disk_eject(d);

    int tracks = hdr[0x30];
    int sides  = hdr[0x31];
    if (sides  < 1) sides  = 1;
    if (sides  > 2) sides  = 2;
    if (tracks > DISK_MAX_TRACKS) tracks = DISK_MAX_TRACKS;

    d->track_count = tracks;
    d->sides       = sides;
    d->inserted    = true;

    /* Normal DSK: fixed track size for all tracks */
    int fixed_track_size = 0;
    if (!extended)
        fixed_track_size = (hdr[0x33] << 8) | hdr[0x32];

    for (int t = 0; t < tracks; t++) {
        for (int s = 0; s < sides; s++) {
            int ts;
            if (extended)
                ts = hdr[0x34 + t * sides + s] * 256;
            else
                ts = fixed_track_size;

            if (ts == 0) continue;  /* missing track in extended DSK */

            if (load_track(&d->track[t][s], f, ts) < 0) {
                fprintf(stderr, "disk: error reading track %d side %d\n", t, s);
                fclose(f);
                return -1;
            }
        }
    }

    fclose(f);
    return 0;
}

/* Standard CPC DATA-format DSK: 40 tracks, 1 side, 9 sectors × 512 bytes.
 * Sector IDs are 0xC1..0xC9 (DATA, not SYSTEM/0x41..). Fill byte 0xE5
 * leaves an empty AMSDOS directory in track-0 sector-0. */
#define BLANK_TRACKS    40
#define BLANK_SPT        9       /* sectors per track */
#define BLANK_SECTOR_SZ  512
#define BLANK_TRACK_SZ  (256 + BLANK_SPT * BLANK_SECTOR_SZ)   /* 4864 */

int disk_create_blank(const char *path) {
    const int TRACKS     = BLANK_TRACKS;
    const int SPT        = BLANK_SPT;
    const int SECTOR_SZ  = BLANK_SECTOR_SZ;
    const int TRACK_SZ   = BLANK_TRACK_SZ;
    const uint8_t FILL   = 0xE5;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "disk: cannot create %s\n", path); return -1; }

    uint8_t hdr[256];
    uint8_t track[BLANK_TRACK_SZ];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr,
           "MV - CPCEMU Disk-File\r\nDisk-Info\r\n",
           34);
    memcpy(hdr + 0x22, "1984        ", 12);   /* creator tag */
    hdr[0x30] = (uint8_t)TRACKS;
    hdr[0x31] = 1;                            /* sides */
    hdr[0x32] = (uint8_t)(TRACK_SZ & 0xFF);
    hdr[0x33] = (uint8_t)(TRACK_SZ >> 8);
    if (fwrite(hdr, 1, 256, f) != 256) goto fail;

    for (int t = 0; t < TRACKS; t++) {
        memset(track, 0, 256);
        memset(track + 256, FILL, SPT * SECTOR_SZ);
        memcpy(track, "Track-Info\r\n", 12);
        track[0x10] = (uint8_t)t;     /* track number */
        track[0x11] = 0;              /* side */
        track[0x14] = 2;              /* sector size code (N=2 → 512) */
        track[0x15] = (uint8_t)SPT;   /* sectors */
        track[0x16] = 0x4E;           /* GAP3 */
        track[0x17] = FILL;           /* filler */
        for (int s = 0; s < SPT; s++) {
            uint8_t *si = track + 0x18 + s * 8;
            si[0] = (uint8_t)t;       /* C */
            si[1] = 0;                /* H */
            si[2] = (uint8_t)(0xC1 + s);  /* R — DATA format */
            si[3] = 2;                /* N */
            si[4] = 0;                /* st1 */
            si[5] = 0;                /* st2 */
            si[6] = (uint8_t)(SECTOR_SZ & 0xFF);
            si[7] = (uint8_t)(SECTOR_SZ >> 8);
        }
        if (fwrite(track, 1, TRACK_SZ, f) != (size_t)TRACK_SZ) goto fail;
    }

    fclose(f);
    return 0;
fail:
    fprintf(stderr, "disk: short write to %s\n", path);
    fclose(f);
    return -1;
}

DiskSector *disk_find_sector(Disk *d, int side, uint8_t C, uint8_t H,
                             uint8_t R, uint8_t N) {
    if (!d->inserted || side >= d->sides) return NULL;
    int t = d->cur_track;
    if (t >= d->track_count) return NULL;
    DiskTrack *tr = &d->track[t][side];
    for (int i = 0; i < tr->sector_count; i++) {
        DiskSector *s = &tr->sectors[i];
        if (s->C == C && s->H == H && s->R == R && s->N == N)
            return s;
    }
    return NULL;
}
