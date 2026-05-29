#include "fat.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ============================================================================
 * Low-level sector I/O
 * ========================================================================= */

static bool sec_read(FatVol *v, u32 lba, void *buf) {
    if (fseek(v->fp, (long)lba * v->bytes_per_sector, SEEK_SET) != 0) return false;
    return fread(buf, v->bytes_per_sector, 1, v->fp) == 1;
}

static bool sec_write(FatVol *v, u32 lba, const void *buf) {
    if (fseek(v->fp, (long)lba * v->bytes_per_sector, SEEK_SET) != 0) return false;
    if (fwrite(buf, v->bytes_per_sector, 1, v->fp) != 1) return false;
    fflush(v->fp);
    return true;
}

/* ============================================================================
 * FAT-entry I/O with a one-sector cache
 * ========================================================================= */

static bool fat_flush_cache(FatVol *v) {
    if (!v->fat_dirty || v->fat_cache_sector == (u32)-1) return true;
    /* Write to all FAT copies for consistency. */
    for (u8 i = 0; i < v->num_fats; i++) {
        u32 lba = v->fat_start_sector + i * v->fat_size_sectors
                  + (v->fat_cache_sector - v->fat_start_sector);
        if (!sec_write(v, lba, v->fat_cache)) return false;
    }
    v->fat_dirty = false;
    return true;
}

static bool fat_load_sector(FatVol *v, u32 lba) {
    if (v->fat_cache_sector == lba) return true;
    if (!fat_flush_cache(v)) return false;
    if (!sec_read(v, lba, v->fat_cache)) {
        v->fat_cache_sector = (u32)-1;
        return false;
    }
    v->fat_cache_sector = lba;
    return true;
}

/* Read FAT entry for `cluster`. Returns next cluster (or end-of-chain code). */
static u32 fat_read_entry(FatVol *v, u32 cluster) {
    u32 off = (v->fat_type == 32) ? cluster * 4u : cluster * 2u;
    u32 sec = v->fat_start_sector + off / v->bytes_per_sector;
    u32 idx = off % v->bytes_per_sector;
    if (!fat_load_sector(v, sec)) return 0;
    if (v->fat_type == 32) {
        u32 e = (u32)v->fat_cache[idx]
              | ((u32)v->fat_cache[idx+1] << 8)
              | ((u32)v->fat_cache[idx+2] << 16)
              | ((u32)v->fat_cache[idx+3] << 24);
        return e & 0x0FFFFFFFu;
    }
    return (u32)v->fat_cache[idx] | ((u32)v->fat_cache[idx+1] << 8);
}

static bool fat_write_entry(FatVol *v, u32 cluster, u32 value) {
    u32 off = (v->fat_type == 32) ? cluster * 4u : cluster * 2u;
    u32 sec = v->fat_start_sector + off / v->bytes_per_sector;
    u32 idx = off % v->bytes_per_sector;
    if (!fat_load_sector(v, sec)) return false;
    if (v->fat_type == 32) {
        u32 old = (u32)v->fat_cache[idx]
                | ((u32)v->fat_cache[idx+1] << 8)
                | ((u32)v->fat_cache[idx+2] << 16)
                | ((u32)v->fat_cache[idx+3] << 24);
        u32 nv  = (old & 0xF0000000u) | (value & 0x0FFFFFFFu);
        v->fat_cache[idx]   = (u8)(nv);
        v->fat_cache[idx+1] = (u8)(nv >> 8);
        v->fat_cache[idx+2] = (u8)(nv >> 16);
        v->fat_cache[idx+3] = (u8)(nv >> 24);
    } else {
        v->fat_cache[idx]   = (u8)value;
        v->fat_cache[idx+1] = (u8)(value >> 8);
    }
    v->fat_dirty = true;
    return true;
}

static bool fat_is_eoc(FatVol *v, u32 entry) {
    if (v->fat_type == 32) return entry >= 0x0FFFFFF8u;
    return entry >= 0xFFF8u;
}

/* Find a free cluster (entry == 0). Returns 0 if the disk is full. */
static u32 fat_alloc_cluster(FatVol *v) {
    for (u32 c = 2; c < v->total_clusters + 2; c++) {
        if (fat_read_entry(v, c) == 0) {
            u32 eoc = (v->fat_type == 32) ? 0x0FFFFFFFu : 0xFFFFu;
            fat_write_entry(v, c, eoc);
            /* Zero-fill the cluster so leftover data isn't visible */
            u8 zero[512];
            memset(zero, 0, sizeof(zero));
            u32 lba = v->data_start_sector + (c - 2) * v->sectors_per_cluster;
            for (u32 i = 0; i < v->sectors_per_cluster; i++)
                sec_write(v, lba + i, zero);
            return c;
        }
    }
    return 0;
}

/* Map cluster index → LBA of its first sector. */
static u32 cluster_to_lba(FatVol *v, u32 cluster) {
    return v->data_start_sector + (cluster - 2) * v->sectors_per_cluster;
}

/* ============================================================================
 * Mount / unmount
 * ========================================================================= */

static u16 rd_u16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

bool fat_mount(FatVol *v, FILE *fp) {
    memset(v, 0, sizeof(*v));
    v->fp = fp;
    v->bytes_per_sector = 512;       /* will be overwritten from BPB */
    v->fat_cache_sector = (u32)-1;

    u8 sec[512];
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(sec, 512, 1, fp) != 1) return false;

    /* Detect MBR vs bare BPB. A FAT boot sector starts with a JMP (EB ?? 90 or
     * E9 ?? ??); an MBR usually starts with 00 or other bytes. We also require
     * the 0x55AA boot signature. */
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;

    bool has_mbr = !(sec[0] == 0xEB || sec[0] == 0xE9);
    u32 part_lba = 0;
    if (has_mbr) {
        /* Walk the 4 partition entries; pick the first usable FAT type. */
        for (int i = 0; i < 4; i++) {
            const u8 *pe = &sec[0x1BE + i * 16];
            u8 type = pe[4];
            if (type == 0x06 || type == 0x0E || type == 0x04 || /* FAT16 */
                type == 0x0B || type == 0x0C ||                  /* FAT32 */
                type == 0x01) {                                  /* FAT12 */
                part_lba = rd_u32(&pe[8]);
                break;
            }
        }
        if (part_lba == 0) return false;
        if (fseek(fp, (long)part_lba * 512, SEEK_SET) != 0) return false;
        if (fread(sec, 512, 1, fp) != 1) return false;
        if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
    }
    v->part_start_lba = part_lba;

    /* BPB fields */
    v->bytes_per_sector     = rd_u16(&sec[11]);
    v->sectors_per_cluster  = sec[13];
    v->reserved_sectors     = rd_u16(&sec[14]);
    v->num_fats             = sec[16];
    v->root_entries         = rd_u16(&sec[17]);
    u16 total16             = rd_u16(&sec[19]);
    u16 fatsz16             = rd_u16(&sec[22]);
    u32 total32             = rd_u32(&sec[32]);
    u32 fatsz32             = rd_u32(&sec[36]);
    v->fat_size_sectors     = fatsz16 ? fatsz16 : fatsz32;
    v->total_sectors        = total16 ? total16 : total32;
    v->root_cluster         = rd_u32(&sec[44]);

    if (v->bytes_per_sector != 512 || v->sectors_per_cluster == 0
            || v->reserved_sectors == 0 || v->num_fats == 0
            || v->fat_size_sectors == 0 || v->total_sectors == 0)
        return false;

    v->bytes_per_cluster = (u32)v->bytes_per_sector * v->sectors_per_cluster;
    v->fat_start_sector  = part_lba + v->reserved_sectors;
    u32 root_dir_sectors = ((u32)v->root_entries * 32 + v->bytes_per_sector - 1)
                            / v->bytes_per_sector;
    v->root_dir_sector   = v->fat_start_sector + v->num_fats * v->fat_size_sectors;
    v->data_start_sector = v->root_dir_sector + root_dir_sectors;
    v->total_clusters    = (v->total_sectors - (v->data_start_sector - part_lba))
                            / v->sectors_per_cluster;

    if (v->total_clusters < 4085)        v->fat_type = 12;
    else if (v->total_clusters < 65525)  v->fat_type = 16;
    else                                  v->fat_type = 32;

    /* FAT12 not implemented (split entries across sectors etc.). */
    if (v->fat_type == 12) return false;

    /* For FAT32, the "root directory" is a regular cluster chain. */
    if (v->fat_type == 32) v->root_dir_sector = 0;

    return true;
}

void fat_unmount(FatVol *v) {
    fat_flush_cache(v);
    v->fp = NULL;
}

u32 fat_free_kb(FatVol *v) {
    u32 free_clusters = 0;
    for (u32 c = 2; c < v->total_clusters + 2; c++)
        if (fat_read_entry(v, c) == 0) free_clusters++;
    u64 bytes = (u64)free_clusters * v->bytes_per_cluster;
    return (u32)(bytes / 1024);
}

/* ============================================================================
 * Directory iteration helpers
 * ========================================================================= */

/* Convert a raw 11-byte 8.3 name from a dir entry into "NAME.EXT" form. */
static void fmt_short_name(const u8 *raw, char *out, size_t outsz) {
    char nm[8], ext[3];
    for (int i = 0; i < 8; i++)
        nm[i] = (raw[i] >= 0x20 && raw[i] < 0x7F) ? (char)raw[i] : ' ';
    for (int i = 0; i < 3; i++)
        ext[i] = (raw[8+i] >= 0x20 && raw[8+i] < 0x7F) ? (char)raw[8+i] : ' ';
    int nl = 8; while (nl > 0 && nm[nl-1] == ' ') nl--;
    int el = 3; while (el > 0 && ext[el-1] == ' ') el--;
    if ((size_t)(nl + 1 + el + 1) > outsz) { out[0] = '\0'; return; }
    int o = 0;
    for (int i = 0; i < nl; i++) out[o++] = nm[i];
    if (el > 0) { out[o++] = '.'; for (int i = 0; i < el; i++) out[o++] = ext[i]; }
    out[o] = '\0';
}

/* Pack a filename into the raw 11-byte 8.3 form. Returns false if the name
 * can't fit (>8 chars before dot or >3 in extension). */
static bool pack_short_name(const char *name, u8 *raw) {
    memset(raw, ' ', 11);
    const char *dot = strrchr(name, '.');
    size_t nl = dot ? (size_t)(dot - name) : strlen(name);
    size_t el = dot ? strlen(dot + 1) : 0;
    if (nl == 0 || nl > 8 || el > 3) return false;
    for (size_t i = 0; i < nl; i++) raw[i]   = (u8)toupper((unsigned char)name[i]);
    for (size_t i = 0; i < el; i++) raw[8+i] = (u8)toupper((unsigned char)dot[1+i]);
    return true;
}

/* Compare two filenames case-insensitively. Returns 0 on match. */
static int name_icmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)toupper((unsigned char)*a);
        char cb = (char)toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

/* Decode LFN slot (32 bytes) into a UCS2->ASCII fragment. Returns chars added. */
static int decode_lfn_slot(const u8 *e, char *out, size_t outsz) {
    /* 5 chars at +1, 6 at +14, 2 at +28 */
    static const int off[] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    int n = 0;
    for (int i = 0; i < 13 && (size_t)n < outsz; i++) {
        u16 c = (u16)e[off[i]] | ((u16)e[off[i]+1] << 8);
        if (c == 0 || c == 0xFFFF) break;
        out[n++] = (c < 0x80) ? (char)c : '?';
    }
    return n;
}

/* Initialise iteration state for the directory at `cluster` (0 = FAT12/16 root). */
static void dir_iter_init(FatDir *d, FatVol *v, u32 cluster) {
    d->vol = v;
    d->first_cluster = cluster;
    d->cur_cluster = cluster;
    d->cur_sector_in_cluster = 0;
    d->cur_root_sector = 0;
    d->entry_in_sector = 0;
    d->at_end = false;
}

/* Read the next 32-byte directory slot into `slot`. Advances internal cursors.
 * Returns false at end of directory. */
static bool dir_iter_next_slot(FatDir *d, u8 *slot,
                               u32 *out_sector_lba, u16 *out_sec_offset) {
    if (d->at_end) return false;
    FatVol *v = d->vol;
    u16 entries_per_sec = v->bytes_per_sector / 32;

    /* Pick the current sector LBA */
    u32 lba;
    if (d->first_cluster == 0) {
        /* FAT12/16 root directory: contiguous sectors at root_dir_sector. */
        u32 root_dir_sectors = ((u32)v->root_entries * 32 + v->bytes_per_sector - 1)
                                / v->bytes_per_sector;
        if (d->cur_root_sector >= root_dir_sectors) { d->at_end = true; return false; }
        lba = v->root_dir_sector + d->cur_root_sector;
    } else {
        lba = cluster_to_lba(v, d->cur_cluster) + d->cur_sector_in_cluster;
    }

    u8 sec[512];
    if (!sec_read(v, lba, sec)) { d->at_end = true; return false; }

    u8 *e = &sec[d->entry_in_sector * 32];
    memcpy(slot, e, 32);
    if (out_sector_lba)  *out_sector_lba = lba;
    if (out_sec_offset)  *out_sec_offset = (u16)(d->entry_in_sector * 32);

    /* Advance cursors */
    d->entry_in_sector++;
    if (d->entry_in_sector >= entries_per_sec) {
        d->entry_in_sector = 0;
        if (d->first_cluster == 0) {
            d->cur_root_sector++;
        } else {
            d->cur_sector_in_cluster++;
            if (d->cur_sector_in_cluster >= v->sectors_per_cluster) {
                d->cur_sector_in_cluster = 0;
                d->cur_cluster = fat_read_entry(v, d->cur_cluster);
                if (fat_is_eoc(v, d->cur_cluster) || d->cur_cluster < 2)
                    d->at_end = true;
            }
        }
    }

    /* Caller decides whether to treat 0x00 as end-of-directory. */
    return true;
}

bool fat_readdir(FatDir *d, char *name, size_t name_sz, u32 *size, bool *is_dir) {
    char lfn[256];
    int  lfn_len = 0;
    for (;;) {
        u8 slot[32];
        if (!dir_iter_next_slot(d, slot, NULL, NULL)) return false;
        if (slot[0] == 0x00) { d->at_end = true; return false; } /* end-of-dir */
        if (slot[0] == 0xE5) { lfn_len = 0; continue; }   /* deleted */
        u8 attr = slot[11];
        if (attr == 0x0F) {
            /* LFN slot: 0x40 set on the last (logical first) entry. Accumulate
             * into `lfn` from the start; ignore checksum mismatches. */
            int seq = slot[0] & 0x1F;
            char chunk[14];
            int  n = decode_lfn_slot(slot, chunk, sizeof(chunk));
            int  pos = (seq - 1) * 13;
            if (pos + n > (int)sizeof(lfn)) { lfn_len = 0; continue; }
            for (int i = 0; i < n; i++) lfn[pos + i] = chunk[i];
            if (pos + n > lfn_len) lfn_len = pos + n;
            continue;
        }
        if (attr & 0x08) { lfn_len = 0; continue; }       /* volume label */

        /* 8.3 entry */
        char shortn[16];
        fmt_short_name(slot, shortn, sizeof(shortn));
        if (shortn[0] == '\0' || shortn[0] == '.') { lfn_len = 0; continue; }

        /* Prefer LFN if present and fits in caller's buffer */
        if (lfn_len > 0 && lfn_len < (int)name_sz) {
            lfn[lfn_len] = '\0';
            snprintf(name, name_sz, "%s", lfn);
        } else {
            snprintf(name, name_sz, "%s", shortn);
        }
        if (is_dir) *is_dir = (attr & 0x10) != 0;
        if (size)   *size   = rd_u32(&slot[28]);
        return true;
    }
}

void fat_closedir(FatDir *d) {
    if (d) free(d);
}

/* Find an entry matching `target` in directory starting at `cluster`. On hit
 * returns true, fills slot_lba/slot_off with the on-disk location of the 8.3
 * entry and `slot` with its 32 bytes. */
static bool find_in_dir(FatVol *v, u32 cluster, const char *target,
                        u32 *slot_lba, u16 *slot_off, u8 *slot_out) {
    FatDir d;
    dir_iter_init(&d, v, cluster);
    char  lfn[256];
    int   lfn_len = 0;
    for (;;) {
        u8 slot[32];
        u32 lba; u16 off;
        if (!dir_iter_next_slot(&d, slot, &lba, &off)) return false;
        if (slot[0] == 0x00) return false;
        if (slot[0] == 0xE5) { lfn_len = 0; continue; }
        u8 attr = slot[11];
        if (attr == 0x0F) {
            int seq = slot[0] & 0x1F;
            char chunk[14];
            int  n = decode_lfn_slot(slot, chunk, sizeof(chunk));
            int  pos = (seq - 1) * 13;
            if (pos + n > (int)sizeof(lfn)) { lfn_len = 0; continue; }
            for (int i = 0; i < n; i++) lfn[pos + i] = chunk[i];
            if (pos + n > lfn_len) lfn_len = pos + n;
            continue;
        }
        if (attr & 0x08) { lfn_len = 0; continue; }
        char shortn[16];
        fmt_short_name(slot, shortn, sizeof(shortn));
        if (shortn[0] == '\0' || shortn[0] == '.') { lfn_len = 0; continue; }
        lfn[lfn_len] = '\0';
        bool match = (name_icmp(shortn, target) == 0)
                   || (lfn_len > 0 && name_icmp(lfn, target) == 0);
        if (match) {
            memcpy(slot_out, slot, 32);
            *slot_lba = lba;
            *slot_off = off;
            return true;
        }
        lfn_len = 0;
    }
}

/* Walk `path` from root and return the directory cluster it refers to (0 for
 * FAT12/16 root). Returns false if any segment doesn't exist or isn't a dir. */
static bool walk_to_dir(FatVol *v, const char *path, u32 *out_cluster) {
    u32 cluster = (v->fat_type == 32) ? v->root_cluster : 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        if (seglen == 0) { p++; continue; }
        char seg[256];
        if (seglen >= sizeof(seg)) return false;
        memcpy(seg, p, seglen); seg[seglen] = '\0';
        u32 lba; u16 off; u8 slot[32];
        if (!find_in_dir(v, cluster, seg, &lba, &off, slot)) return false;
        if (!(slot[11] & 0x10)) return false;
        cluster = ((u32)rd_u16(&slot[20]) << 16) | rd_u16(&slot[26]);
        p += seglen;
        while (*p == '/') p++;
    }
    *out_cluster = cluster;
    return true;
}

FatDir *fat_opendir(FatVol *v, const char *path) {
    u32 cluster;
    if (!walk_to_dir(v, path, &cluster)) return NULL;
    FatDir *d = (FatDir *)calloc(1, sizeof(FatDir));
    if (!d) return NULL;
    dir_iter_init(d, v, cluster);
    return d;
}

bool fat_dir_exists(FatVol *v, const char *path) {
    u32 c;
    return walk_to_dir(v, path, &c);
}

/* ============================================================================
 * File open / read / write / close
 * ========================================================================= */

/* Split "path/to/file.bas" into parent dir cluster + leaf name. */
static bool split_parent(FatVol *v, const char *path, u32 *parent_cluster,
                         char *leaf, size_t leaf_sz) {
    const char *last = strrchr(path, '/');
    if (!last) {
        snprintf(leaf, leaf_sz, "%s", path);
        *parent_cluster = (v->fat_type == 32) ? v->root_cluster : 0;
        return true;
    }
    snprintf(leaf, leaf_sz, "%s", last + 1);
    if (last == path) {
        *parent_cluster = (v->fat_type == 32) ? v->root_cluster : 0;
        return true;
    }
    char parent[512];
    size_t pl = (size_t)(last - path);
    if (pl >= sizeof(parent)) return false;
    memcpy(parent, path, pl); parent[pl] = '\0';
    return walk_to_dir(v, parent, parent_cluster);
}

/* Find a free 8.3 dir entry in `dir_cluster`. Returns LBA + offset; allocates
 * a new cluster if needed (root FAT12/16 can't grow). */
static bool find_free_dir_slot(FatVol *v, u32 dir_cluster,
                               u32 *out_lba, u16 *out_off) {
    FatDir d;
    dir_iter_init(&d, v, dir_cluster);
    for (;;) {
        u8 slot[32];
        u32 lba; u16 off;
        if (!dir_iter_next_slot(&d, slot, &lba, &off)) break;
        if (slot[0] == 0xE5 || slot[0] == 0x00) {
            *out_lba = lba;
            *out_off = off;
            return true;
        }
    }
    /* TODO: grow the directory by allocating a new cluster (FAT32 dirs).
     * For now this only works if there's a free slot in the existing area. */
    return false;
}

FatFile *fat_open(FatVol *v, const char *path, bool write_create) {
    char leaf[256];
    u32  parent;
    if (!split_parent(v, path, &parent, leaf, sizeof(leaf))) return NULL;

    u32 lba = 0; u16 off = 0; u8 slot[32];
    bool exists = find_in_dir(v, parent, leaf, &lba, &off, slot);

    if (!write_create) {
        if (!exists || (slot[11] & 0x18)) return NULL;
        FatFile *f = (FatFile *)calloc(1, sizeof(FatFile));
        if (!f) return NULL;
        f->vol           = v;
        f->first_cluster = ((u32)rd_u16(&slot[20]) << 16) | rd_u16(&slot[26]);
        f->file_size     = rd_u32(&slot[28]);
        f->dir_sector    = lba;
        f->dir_offset    = off;
        return f;
    }

    /* Write/create: truncate or create. */
    u8 sec[512];
    if (exists) {
        /* Truncate: free the cluster chain. */
        u32 c = ((u32)rd_u16(&slot[20]) << 16) | rd_u16(&slot[26]);
        while (c >= 2 && !fat_is_eoc(v, c)) {
            u32 n = fat_read_entry(v, c);
            fat_write_entry(v, c, 0);
            c = n;
        }
        if (!sec_read(v, lba, sec)) return NULL;
        u8 *e = &sec[off];
        e[20] = e[21] = e[26] = e[27] = 0;       /* first cluster = 0 */
        e[28] = e[29] = e[30] = e[31] = 0;       /* size = 0 */
        if (!sec_write(v, lba, sec)) return NULL;
        fat_flush_cache(v);
    } else {
        u8 raw[11];
        if (!pack_short_name(leaf, raw)) return NULL;
        u32 dlba; u16 doff;
        if (!find_free_dir_slot(v, parent, &dlba, &doff)) return NULL;
        if (!sec_read(v, dlba, sec)) return NULL;
        u8 *e = &sec[doff];
        memset(e, 0, 32);
        memcpy(e, raw, 11);
        e[11] = 0x20; /* archive attr */
        if (!sec_write(v, dlba, sec)) return NULL;
        lba = dlba; off = doff;
    }

    FatFile *f = (FatFile *)calloc(1, sizeof(FatFile));
    if (!f) return NULL;
    f->vol           = v;
    f->first_cluster = 0;
    f->file_size     = 0;
    f->dir_sector    = lba;
    f->dir_offset    = off;
    f->write_mode    = true;
    f->modified      = true;
    return f;
}

/* Walk the cluster chain to the cluster containing byte offset `offset`. */
static u32 cluster_at_offset(FatVol *v, u32 first, u32 offset) {
    if (first < 2) return 0;
    u32 c = first;
    u32 cnt = offset / v->bytes_per_cluster;
    for (u32 i = 0; i < cnt; i++) {
        c = fat_read_entry(v, c);
        if (fat_is_eoc(v, c) || c < 2) return 0;
    }
    return c;
}

u32 fat_read(FatFile *f, void *buf, u32 size) {
    if (!f || f->write_mode) return 0;
    FatVol *v = f->vol;
    u8 *out = (u8 *)buf;
    u32 got = 0;
    if (f->file_offset >= f->file_size) return 0;
    if (size > f->file_size - f->file_offset) size = f->file_size - f->file_offset;

    while (got < size) {
        u32 c = cluster_at_offset(v, f->first_cluster, f->file_offset);
        if (c < 2) break;
        u32 within  = f->file_offset % v->bytes_per_cluster;
        u32 chunk   = v->bytes_per_cluster - within;
        if (chunk > size - got) chunk = size - got;

        u32 lba = cluster_to_lba(v, c) + within / v->bytes_per_sector;
        u32 sec_within = within % v->bytes_per_sector;
        u8  sec[512];
        if (!sec_read(v, lba, sec)) break;
        u32 first_take = v->bytes_per_sector - sec_within;
        if (first_take > chunk) first_take = chunk;
        memcpy(out + got, &sec[sec_within], first_take);
        got            += first_take;
        f->file_offset += first_take;
        chunk          -= first_take;
        lba++;
        while (chunk > 0) {
            u32 take = chunk > v->bytes_per_sector ? v->bytes_per_sector : chunk;
            if (!sec_read(v, lba, sec)) goto done;
            memcpy(out + got, sec, take);
            got            += take;
            f->file_offset += take;
            chunk          -= take;
            lba++;
        }
    }
done:
    return got;
}

u32 fat_write(FatFile *f, const void *buf, u32 size) {
    if (!f || !f->write_mode) return 0;
    FatVol *v = f->vol;
    const u8 *in = (const u8 *)buf;
    u32 wrote = 0;

    while (wrote < size) {
        u32 cluster_idx = f->file_offset / v->bytes_per_cluster;
        u32 within      = f->file_offset % v->bytes_per_cluster;

        /* Find / allocate the cluster for this offset. */
        u32 c = f->first_cluster;
        if (c < 2) {
            /* Empty file: allocate first cluster. */
            c = fat_alloc_cluster(v);
            if (c == 0) break;
            f->first_cluster = c;
        }
        for (u32 i = 0; i < cluster_idx; i++) {
            u32 n = fat_read_entry(v, c);
            if (fat_is_eoc(v, n) || n < 2) {
                /* Extend chain. */
                n = fat_alloc_cluster(v);
                if (n == 0) goto done;
                fat_write_entry(v, c, n);
            }
            c = n;
        }

        /* Write into the cluster. */
        u32 chunk = v->bytes_per_cluster - within;
        if (chunk > size - wrote) chunk = size - wrote;

        u32 sec_within = within % v->bytes_per_sector;
        u32 lba        = cluster_to_lba(v, c) + within / v->bytes_per_sector;
        u8  sec[512];
        if (sec_within != 0 || chunk < v->bytes_per_sector) {
            if (!sec_read(v, lba, sec)) break;
        } else {
            memset(sec, 0, sizeof(sec));
        }
        u32 first_take = v->bytes_per_sector - sec_within;
        if (first_take > chunk) first_take = chunk;
        memcpy(&sec[sec_within], in + wrote, first_take);
        if (!sec_write(v, lba, sec)) break;
        wrote          += first_take;
        f->file_offset += first_take;
        chunk          -= first_take;
        lba++;

        while (chunk > 0) {
            u32 take = chunk > v->bytes_per_sector ? v->bytes_per_sector : chunk;
            if (take < v->bytes_per_sector) {
                if (!sec_read(v, lba, sec)) goto done;
            }
            memcpy(sec, in + wrote, take);
            if (take < v->bytes_per_sector) {
                if (!sec_write(v, lba, sec)) goto done;
            } else {
                if (!sec_write(v, lba, in + wrote)) goto done;
            }
            wrote          += take;
            f->file_offset += take;
            chunk          -= take;
            lba++;
        }
    }
done:
    if (f->file_offset > f->file_size) f->file_size = f->file_offset;
    f->modified = true;
    fat_flush_cache(v);
    return wrote;
}

bool fat_seek(FatFile *f, u32 pos) {
    if (!f) return false;
    f->file_offset = pos;
    return true;
}

u32 fat_tell(FatFile *f)      { return f ? f->file_offset : 0; }
u32 fat_file_size(FatFile *f) { return f ? f->file_size   : 0; }

bool fat_write_dir_entry(FatVol *v, u32 sector, u16 byte_offset,
                         const u8 entry[32]) {
    if (!v || !v->fp || byte_offset + 32 > v->bytes_per_sector) return false;
    u8 sec[512];
    if (!sec_read(v, sector, sec)) return false;
    memcpy(&sec[byte_offset], entry, 32);
    if (!sec_write(v, sector, sec)) return false;
    fat_flush_cache(v);
    return true;
}

void fat_close(FatFile *f) {
    if (!f) return;
    if (f->write_mode && f->modified) {
        /* Persist size + first-cluster into the directory entry. */
        u8 sec[512];
        if (sec_read(f->vol, f->dir_sector, sec)) {
            u8 *e = &sec[f->dir_offset];
            e[20] = (u8)(f->first_cluster >> 16);
            e[21] = (u8)(f->first_cluster >> 24);
            e[26] = (u8)(f->first_cluster);
            e[27] = (u8)(f->first_cluster >> 8);
            e[28] = (u8)(f->file_size);
            e[29] = (u8)(f->file_size >> 8);
            e[30] = (u8)(f->file_size >> 16);
            e[31] = (u8)(f->file_size >> 24);
            sec_write(f->vol, f->dir_sector, sec);
        }
        fat_flush_cache(f->vol);
    }
    free(f);
}
