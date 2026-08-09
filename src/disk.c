#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

static int load_track(DiskTrack *tr, FILE *f, int track_size, long track_file_offset) {
    if (track_size < 256) return 0;
    tr->file_offset = track_file_offset;

    uint8_t hdr[256];
    if (fread(hdr, 1, 256, f) != 256) return -1;

    /* "Track-Info" marker */
    if (memcmp(hdr, "Track-Info", 10) != 0) return 0;

    int spt  = hdr[0x15];
    if (spt > DISK_MAX_SECTORS) spt = DISK_MAX_SECTORS;
    tr->sector_count = spt;

    /* Sector data follows the 256-byte header */
    long data_file_offset = track_file_offset + 256;
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
        sec->file_offset = data_file_offset + offset;
        offset += sz;
    }
    return 0;
}

int disk_load(Disk *d, const char *path) {
    bool write_protected = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        write_protected = true;
    }
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
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->write_protected = write_protected;

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

            long track_file_offset = ftell(f);
            if (track_file_offset < 0 ||
                load_track(&d->track[t][s], f, ts, track_file_offset) < 0) {
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

bool disk_ensure_dsk_extension(char *path, size_t capacity) {
    static const char suffix[] = ".dsk";
    size_t len;

    if (!path || capacity == 0 || path[0] == '\0')
        return false;

    len = strlen(path);
    if (len >= sizeof(suffix) - 1) {
        const char *tail = path + len - (sizeof(suffix) - 1);
        if (tail[0] == '.' &&
            (tail[1] == 'd' || tail[1] == 'D') &&
            (tail[2] == 's' || tail[2] == 'S') &&
            (tail[3] == 'k' || tail[3] == 'K'))
            return true;
    }

    if (len >= capacity || capacity - len < sizeof(suffix))
        return false;

    memcpy(path + len, suffix, sizeof(suffix));
    return true;
}

static const DiskSector *disk_sector_by_id(const Disk *d, int track, int side,
                                            uint8_t id) {
    if (!d || !d->inserted || track < 0 || track >= d->track_count ||
        side < 0 || side >= d->sides)
        return NULL;
    const DiskTrack *tr = &d->track[track][side];
    for (int i = 0; i < tr->sector_count; i++)
        if (tr->sectors[i].R == id)
            return &tr->sectors[i];
    return NULL;
}

static bool disk_read_directory_sector(const Disk *d, int track, uint8_t id,
                                       uint8_t *out) {
    const DiskSector *sec = disk_sector_by_id(d, track, 0, id);
    if (!sec || sec->size < 512)
        return false;
    const DiskTrack *tr = &d->track[track][0];
    if (!tr->data || sec->offset < 0 || sec->offset + 512 > tr->data_size)
        return false;
    memcpy(out, tr->data + sec->offset, 512);
    return true;
}

static bool disk_directory_geometry(const Disk *d, int *track,
                                    uint8_t *first_sector) {
    if (disk_sector_by_id(d, 0, 0, 0xC1)) {
        *track = 0;             /* CPC DATA format */
        *first_sector = 0xC1;
        return true;
    }
    if (disk_sector_by_id(d, 0, 0, 0x41)) {
        *track = 2;             /* CPC SYSTEM format: two reserved tracks */
        *first_sector = 0x41;
        return true;
    }
    if (disk_sector_by_id(d, 0, 0, 0x01)) {
        *track = 1;             /* IBM format: one reserved track */
        *first_sector = 0x01;
        return true;
    }
    return false;
}

static bool cpm_directory_name(const uint8_t *entry,
                               char out[DISK_AMSDOS_NAME_MAX]) {
    char name[9];
    char ext[4];
    int name_len = 8;
    int ext_len = 3;

    for (int i = 0; i < 8; i++) {
        unsigned char c = entry[1 + i] & 0x7F;
        if (c != ' ' && (c < 0x21 || c > 0x7E || c == '.'))
            return false;
        name[i] = (char)c;
    }
    name[8] = '\0';
    while (name_len > 0 && name[name_len - 1] == ' ')
        name_len--;
    if (name_len == 0)
        return false;

    for (int i = 0; i < 3; i++) {
        unsigned char c = entry[9 + i] & 0x7F;
        if (c != ' ' && (c < 0x21 || c > 0x7E || c == '.'))
            return false;
        ext[i] = (char)c;
    }
    ext[3] = '\0';
    while (ext_len > 0 && ext[ext_len - 1] == ' ')
        ext_len--;

    int pos = 0;
    for (int i = 0; i < name_len; i++)
        out[pos++] = (char)toupper((unsigned char)name[i]);
    if (ext_len > 0) {
        out[pos++] = '.';
        for (int i = 0; i < ext_len; i++)
            out[pos++] = (char)toupper((unsigned char)ext[i]);
    }
    out[pos] = '\0';
    return true;
}

static int directory_entry_compare(const void *a_, const void *b_) {
    const DiskDirectoryEntry *a = a_;
    const DiskDirectoryEntry *b = b_;
    if (a->user != b->user)
        return (int)a->user - (int)b->user;
    return strcmp(a->name, b->name);
}

int disk_list_directory(const Disk *d, DiskDirectoryEntry *entries,
                        int capacity) {
    uint8_t directory[64 * 32];
    int dir_track;
    uint8_t first_sector;

    if (!d || !d->inserted || !entries || capacity <= 0 ||
        !disk_directory_geometry(d, &dir_track, &first_sector))
        return -1;
    if (dir_track >= d->track_count)
        return -1;
    for (int i = 0; i < 4; i++)
        if (!disk_read_directory_sector(d, dir_track,
                                        (uint8_t)(first_sector + i),
                                        directory + i * 512))
            return -1;

    int count = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *raw = directory + i * 32;
        if (raw[0] > 15)
            continue;

        char name[DISK_AMSDOS_NAME_MAX];
        if (!cpm_directory_name(raw, name))
            continue;

        int found = -1;
        for (int j = 0; j < count; j++) {
            if (entries[j].user == raw[0] && !strcmp(entries[j].name, name)) {
                found = j;
                break;
            }
        }
        if (found < 0) {
            if (count >= capacity)
                continue;
            found = count++;
            entries[found].user = raw[0];
            snprintf(entries[found].name, sizeof(entries[found].name),
                     "%s", name);
            entries[found].size = 0;
        }

        unsigned extent = (raw[12] & 0x1F) | ((raw[14] & 0x3F) << 5);
        unsigned records = raw[15] > 128 ? 128 : raw[15];
        uint32_t end = (uint32_t)(extent * 128U + records) * 128U;
        if (end > entries[found].size)
            entries[found].size = end;
    }

    qsort(entries, (size_t)count, sizeof(*entries), directory_entry_compare);
    return count;
}

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

int disk_write_sector(Disk *d, DiskSector *sec, const uint8_t *data, int len) {
    if (!d || !sec || !data || !d->inserted || d->write_protected ||
        !d->path[0] || sec->file_offset < 0 || len < 0)
        return -1;

    if (len > sec->size) len = sec->size;

    FILE *f = fopen(d->path, "r+b");
    if (!f) {
        d->write_protected = true;
        return -1;
    }

    int rc = 0;
    if (fseek(f, sec->file_offset, SEEK_SET) != 0 ||
        fwrite(data, 1, (size_t)len, f) != (size_t)len ||
        fflush(f) != 0) {
        rc = -1;
        d->write_protected = true;
    }

    fclose(f);
    return rc;
}

int disk_format_track(Disk *d, int side, const uint8_t *chrn, int count,
                      uint8_t default_N, uint8_t gap3, uint8_t filler) {
    if (!d || !chrn || !d->inserted || d->write_protected || !d->path[0] ||
        side < 0 || side >= d->sides || d->cur_track >= d->track_count ||
        count <= 0 || count > DISK_MAX_SECTORS)
        return -1;

    DiskTrack *tr = &d->track[d->cur_track][side];
    if (!tr->data || tr->data_size <= 0 || tr->file_offset < 0)
        return -1;

    DiskSector sectors[DISK_MAX_SECTORS];
    memset(sectors, 0, sizeof(sectors));

    int offset = 0;
    for (int i = 0; i < count; i++) {
        uint8_t N = chrn[i * 4 + 3];
        if (N > 7) return -1;
        int size = 128 << N;
        if (offset + size > tr->data_size) return -1;

        DiskSector *sec = &sectors[i];
        sec->C = chrn[i * 4 + 0];
        sec->H = chrn[i * 4 + 1];
        sec->R = chrn[i * 4 + 2];
        sec->N = N;
        sec->st1 = 0;
        sec->st2 = 0;
        sec->offset = offset;
        sec->file_offset = tr->file_offset + 256 + offset;
        sec->size = size;
        offset += size;
    }

    uint8_t hdr[256];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "Track-Info\r\n", 12);
    hdr[0x10] = (uint8_t)d->cur_track;
    hdr[0x11] = (uint8_t)side;
    hdr[0x14] = default_N;
    hdr[0x15] = (uint8_t)count;
    hdr[0x16] = gap3;
    hdr[0x17] = filler;
    for (int i = 0; i < count; i++) {
        uint8_t *si = hdr + 0x18 + i * 8;
        const DiskSector *sec = &sectors[i];
        si[0] = sec->C;
        si[1] = sec->H;
        si[2] = sec->R;
        si[3] = sec->N;
        si[4] = sec->st1;
        si[5] = sec->st2;
        si[6] = (uint8_t)(sec->size & 0xFF);
        si[7] = (uint8_t)(sec->size >> 8);
    }

    uint8_t *data = malloc((size_t)tr->data_size);
    if (!data) return -1;
    memset(data, filler, (size_t)tr->data_size);

    FILE *f = fopen(d->path, "r+b");
    if (!f) {
        d->write_protected = true;
        free(data);
        return -1;
    }

    int rc = 0;
    if (fseek(f, tr->file_offset, SEEK_SET) != 0 ||
        fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        fwrite(data, 1, (size_t)tr->data_size, f) != (size_t)tr->data_size ||
        fflush(f) != 0) {
        rc = -1;
        d->write_protected = true;
    }
    fclose(f);

    if (rc == 0) {
        memcpy(tr->data, data, (size_t)tr->data_size);
        memcpy(tr->sectors, sectors, sizeof(sectors));
        tr->sector_count = count;
    }

    free(data);
    return rc;
}
