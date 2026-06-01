#include "ch376.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

int ch376_trace = 0;
int ch376_disable_disk_read = 0;

/* CH376 USB host controller — partial emulation for Albireo.
 *
 * Implements the command set used by UNIDOS for FAT file I/O. The chip's
 * internal FAT logic is backed by src/fat.c against the host image file.
 *
 * Result codes (subset). */
#define USB_INT_SUCCESS    0x14
#define USB_INT_CONNECT    0x15
#define USB_INT_DISCONNECT 0x16
#define USB_INT_DISK_READ  0x1D
#define USB_INT_DISK_WRITE 0x1E
#define USB_INT_DISK_ERR   0x1F
#define ERR_OPEN_DIR       0x41
#define ERR_MISS_FILE      0x42
#define ERR_FOUND_NAME     0x43

#define CH_CHUNK 255   /* max bytes per RD_USB_DATA0 / WR_REQ_DATA chunk */

/* -------------------------------------------------------------------------- */

static void clear_pending(CH376 *ch) {
    ch->pending_cmd = 0;
    ch->param_count = 0;
    ch->param_needed = 0;
}

static void set_response(CH376 *ch, const u8 *data, int len) {
    if (len > (int)sizeof(ch->resp)) len = sizeof(ch->resp);
    if (data) memcpy(ch->resp, data, len);
    ch->resp_len = len;
    ch->resp_pos = 0;
}

static void set_oneshot(CH376 *ch, u8 byte) {
    ch->oneshot = byte;
    ch->oneshot_valid = true;
}

static void raise_int(CH376 *ch, u8 code) {
    ch->int_status  = code;
    ch->int_pending = true;
    if (ch376_trace)
        fprintf(stderr, "[albireo]   int=%02X\n", code);
}

static const char *cmd_name(u8 c) {
    switch (c) {
    case 0x01: return "GET_IC_VER";
    case 0x05: return "RESET_ALL";
    case 0x06: return "CHECK_EXIST";
    case 0x0B: return "SET_FREQ";
    case 0x13: return "SET_USB_ADDR";
    case 0x15: return "SET_USB_MODE";
    case 0x22: return "GET_STATUS";
    case 0x27: return "RD_USB_DATA0";
    case 0x28: return "RD_USB_DATA";
    case 0x2D: return "WR_REQ_DATA";
    case 0x2F: return "SET_FILE_NAME";
    case 0x30: return "DISK_CONNECT";
    case 0x31: return "DISK_MOUNT";
    case 0x32: return "FILE_OPEN";
    case 0x33: return "FILE_ENUM_GO";
    case 0x34: return "FILE_CREATE";
    case 0x35: return "FILE_ERASE";
    case 0x36: return "FILE_CLOSE";
    case 0x37: return "DIR_INFO_READ";
    case 0x38: return "DIR_INFO_SAVE";
    case 0x39: return "BYTE_LOCATE";
    case 0x3A: return "BYTE_READ";
    case 0x3B: return "BYTE_RD_GO";
    case 0x3C: return "BYTE_WRITE";
    case 0x3D: return "BYTE_WR_GO";
    case 0x3E: return "DISK_CAPACITY";
    case 0x3F: return "DISK_QUERY";
    case 0x40: return "DIR_CREATE";
    case 0x4A: return "SEC_LOCATE";
    case 0x4B: return "SEC_READ";
    case 0x4C: return "SEC_WRITE";
    default:   return "?";
    }
}

/* Normalise: backslashes → slashes, ensure leading slash, uppercase. */
static void normalise_path(const char *in, char *out, size_t sz) {
    size_t i = 0, j = 0;
    if (in[0] != '/' && in[0] != '\\') {
        out[j++] = '/';
    }
    while (in[i] && j + 1 < sz) {
        char c = in[i++];
        if (c == '\\') c = '/';
        out[j++] = (char)toupper((unsigned char)c);
    }
    out[j] = '\0';
}

/* Split a path into parent dir and leaf. parent gets the dir incl trailing
 * slash stripped (or "/" if at root). */
static void split_path(const char *path, char *parent, size_t psz,
                       char *leaf, size_t lsz) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        snprintf(parent, psz, "/");
        snprintf(leaf, lsz, "%.63s", slash ? slash + 1 : path);
    } else {
        size_t plen = (size_t)(slash - path);
        if (plen >= psz) plen = psz - 1;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        snprintf(leaf, lsz, "%.63s", slash + 1);
    }
}

static bool name_has_wildcard(const char *s) {
    return strchr(s, '*') || strchr(s, '?');
}

/* Collapse the padded 8.3 form UNIDOS hands us ("TCPTEST .BAS",
 * "SYM     .   ") into the compact form fat_open expects
 * ("TCPTEST.BAS", "SYM"). Operates on the *leaf* portion only;
 * parent directories should already be normal. */
static void compact_8_3_leaf(char *leaf) {
    char *dot = strchr(leaf, '.');
    if (!dot) {
        /* Trim trailing spaces in a dot-less name. */
        size_t n = strlen(leaf);
        while (n > 0 && leaf[n - 1] == ' ') leaf[--n] = '\0';
        return;
    }
    /* Trim trailing spaces in the base name (before the dot). */
    char *p = dot;
    while (p > leaf && p[-1] == ' ') p--;
    /* Trim trailing spaces in the extension (after the dot). */
    char *ext_end = dot + 1 + strlen(dot + 1);
    while (ext_end > dot + 1 && ext_end[-1] == ' ') ext_end--;
    if (ext_end == dot + 1) {
        /* Extension is empty/all spaces — drop the dot too. */
        *p = '\0';
    } else {
        size_t ext_len = (size_t)(ext_end - (dot + 1));
        *p++ = '.';
        memmove(p, dot + 1, ext_len);
        p[ext_len] = '\0';
    }
}

/* Glob-match 8.3 filename against pattern. Both already uppercased. */
static bool wildcard_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            while (*name) {
                if (wildcard_match(pat, name)) return true;
                name++;
            }
            return false;
        }
        if (!*name) return false;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*name))
            return false;
        pat++; name++;
    }
    return *name == '\0';
}

/* Format `name` (e.g., "FOO.BAR") into 11-byte 8.3 space-padded buffer. */
static void format_8_3(const char *name, u8 out[11]) {
    memset(out, ' ', 11);
    int i = 0;
    while (name[i] && name[i] != '.' && i < 8) {
        out[i] = (u8)toupper((unsigned char)name[i]);
        i++;
    }
    const char *dot = strchr(name, '.');
    if (dot) {
        for (int e = 0; e < 3 && dot[1 + e]; e++)
            out[8 + e] = (u8)toupper((unsigned char)dot[1 + e]);
    }
}

/* Build a 32-byte FAT dir entry from name/size/is_dir. */
static void make_dir_entry(u8 out[32], const char *name, u32 size, bool is_dir) {
    memset(out, 0, 32);
    format_8_3(name, out);
    out[11] = is_dir ? 0x10 : 0x20;
    out[0x1C] = (u8)(size       & 0xFF);
    out[0x1D] = (u8)((size >>  8) & 0xFF);
    out[0x1E] = (u8)((size >> 16) & 0xFF);
    out[0x1F] = (u8)((size >> 24) & 0xFF);
}

/* -------------------------------------------------------------------------- */
/* File backend helpers                                                       */

static void close_file(CH376 *ch) {
    if (ch->file) { fat_close(ch->file); ch->file = NULL; }
    ch->file_writing  = false;
    ch->reading = ch->writing = false;
    ch->bytes_remaining = 0;
}

static void close_enum(CH376 *ch) {
    if (ch->enum_dir) { fat_closedir(ch->enum_dir); ch->enum_dir = NULL; }
    ch->have_dir_entry = false;
}

/* Stage the 32-byte FAT dir entry as a length-prefixed 33-byte payload so
 * UNIDOS's CatDecodeEntry (read 32 via RD_USB_DATA0, then INI 11+1 bytes)
 * gets the expected `32, <entry>` format. */
static void serve_dir_entry(CH376 *ch) {
    ch->resp[0] = 32;
    memcpy(&ch->resp[1], ch->last_dir_entry, 32);
    ch->resp_len = 33;
    ch->resp_pos = 0;
}

/* Try to satisfy FILE_OPEN. Returns the result code to send via interrupt. */
static u8 do_file_open(CH376 *ch) {
    close_file(ch);
    close_enum(ch);

    if (!ch->mounted) return USB_INT_DISK_ERR;

    char parent[256], leaf[64];
    split_path(ch->filename, parent, sizeof(parent), leaf, sizeof(leaf));

    /* Wildcard → start enumeration. */
    if (name_has_wildcard(leaf)) {
        ch->enum_dir = fat_opendir(&ch->vol, parent);
        if (!ch->enum_dir) return ERR_OPEN_DIR;
        snprintf(ch->enum_pattern, sizeof(ch->enum_pattern), "%.15s", leaf);
        snprintf(ch->enum_parent,  sizeof(ch->enum_parent),  "%s", parent);

        char name[16]; u32 size; bool is_dir;
        while (fat_readdir(ch->enum_dir, name, sizeof(name), &size, &is_dir)) {
            if (wildcard_match(leaf, name)) {
                make_dir_entry(ch->last_dir_entry, name, size, is_dir);
                ch->have_dir_entry = true;
                serve_dir_entry(ch);
                return USB_INT_DISK_READ;
            }
        }
        close_enum(ch);
        return ERR_MISS_FILE;
    }

    /* Exact name → directory? */
    if (fat_dir_exists(&ch->vol, ch->filename))
        return ERR_OPEN_DIR;

    FatFile *f = fat_open(&ch->vol, ch->filename, false);
    if (!f) return ERR_MISS_FILE;
    ch->file = f;
    ch->bytes_remaining = 0;

    /* Populate last_dir_entry so a subsequent DIR_INFO_READ — which UNIDOS
     * calls right after FILE_OPEN to fetch size/attributes — returns the
     * 32-byte FAT entry instead of ERR_MISS_FILE. Walk the parent directory
     * once and match the leaf name (case-insensitive, matching fat.c's lookup). */
    FatDir *d = fat_opendir(&ch->vol, parent);
    if (d) {
        char ename[16]; u32 esize; bool eis_dir;
        while (fat_readdir(d, ename, sizeof(ename), &esize, &eis_dir)) {
            if (strcasecmp(ename, leaf) == 0) {
                make_dir_entry(ch->last_dir_entry, ename, esize, eis_dir);
                ch->have_dir_entry = true;
                break;
            }
        }
        fat_closedir(d);
    }
    /* The FatFile carries the on-disk location of its own directory entry —
     * use that for WR_OFS_DATA / DIR_INFO_SAVE writes (UNIDOS's rename idiom). */
    ch->dir_entry_sector   = ch->file->dir_sector;
    ch->dir_entry_offset   = ch->file->dir_offset;
    ch->dir_entry_writable = (ch->file->dir_sector != 0);
    return USB_INT_SUCCESS;
}

static u8 do_file_enum_go(CH376 *ch) {
    if (!ch->enum_dir) return ERR_MISS_FILE;
    char name[16]; u32 size; bool is_dir;
    while (fat_readdir(ch->enum_dir, name, sizeof(name), &size, &is_dir)) {
        if (wildcard_match(ch->enum_pattern, name)) {
            make_dir_entry(ch->last_dir_entry, name, size, is_dir);
            ch->have_dir_entry = true;
            serve_dir_entry(ch);
            return USB_INT_DISK_READ;
        }
    }
    close_enum(ch);
    return ERR_MISS_FILE;
}

static u8 do_file_create(CH376 *ch) {
    close_file(ch);
    if (!ch->mounted) return USB_INT_DISK_ERR;
    FatFile *f = fat_open(&ch->vol, ch->filename, true);
    if (!f) return USB_INT_DISK_ERR;
    ch->file = f;
    ch->file_writing = true;
    return USB_INT_SUCCESS;
}

static u8 do_byte_read(CH376 *ch) {
    if (!ch->file) return USB_INT_DISK_ERR;
    u32 want = ch->bytes_remaining;
    if (want > CH_CHUNK) want = CH_CHUNK;
    u32 got = 0;
    if (want > 0) {
        got = fat_read(ch->file, &ch->resp[1], want);
        ch->bytes_remaining -= got;
    }
    /* Always update resp[] — even when emitting a 0-byte "end of stream"
     * chunk — so RD_USB_DATA0 doesn't return stale length from the prior
     * chunk. UNIDOS's read loop exits on (status=SUCCESS && chunk len=0). */
    ch->resp[0] = (u8)got;
    ch->resp_len = (int)got + 1;
    ch->resp_pos = 0;
    if (got == 0) {
        ch->reading = false;
        return USB_INT_SUCCESS;
    }
    ch->reading = (ch->bytes_remaining > 0);
    return USB_INT_DISK_READ;
}

/* -------------------------------------------------------------------------- */
/* Public lifecycle                                                           */

void ch376_init(CH376 *ch) {
    memset(ch, 0, sizeof(*ch));
}

void ch376_reset(CH376 *ch) {
    close_file(ch);
    close_enum(ch);
    ch->pending_cmd = 0;
    ch->param_count = ch->param_needed = 0;
    ch->resp_len = ch->resp_pos = 0;
    ch->wbuf_len = ch->wbuf_pos = 0;
    ch->int_status = 0;
    ch->int_pending = false;
    ch->oneshot_valid = false;
    ch->bytes_remaining = 0;
    ch->reading = ch->writing = false;
    ch->filename[0] = '\0';
    ch->mouse_dx = ch->mouse_dy = 0;
    ch->mouse_buttons = 0;
    ch->sec_reading = ch->sec_writing = false;
    ch->sec_remaining = 0;
    ch->sec_pos = 0;
}

void ch376_open(CH376 *ch, const char *path) {
    ch376_close(ch);
    if (!path || !*path) return;
    snprintf(ch->path, sizeof(ch->path), "%s", path);
    ch->fp = fopen(path, "r+b");
    if (!ch->fp) ch->fp = fopen(path, "rb");
    if (!ch->fp) return;
    ch->mounted = fat_mount(&ch->vol, ch->fp);
}

void ch376_close(CH376 *ch) {
    close_file(ch);
    close_enum(ch);
    if (ch->mounted) { fat_unmount(&ch->vol); ch->mounted = false; }
    if (ch->fp) { fclose(ch->fp); ch->fp = NULL; }
    ch->path[0] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Command dispatch                                                           */

/* Number of fixed parameter bytes for each known command. -1 = until NUL.
 * 0 = execute immediately on command write. -2 = handled out-of-band. */
static int param_count_for(u8 cmd) {
    switch (cmd) {
    case 0x01: return 0;   /* GET_IC_VER (result in DATA port) */
    case 0x05: return 0;   /* RESET_ALL */
    case 0x06: return 1;   /* CHECK_EXIST */
    case 0x0B: return 1;   /* SET_FREQ — accept & ignore */
    case 0x13: return 1;   /* SET_USB_ADDR */
    case 0x15: return 1;   /* SET_USB_MODE */
    case 0x22: return 0;   /* GET_STATUS */
    case 0x27: return 0;   /* RD_USB_DATA0 — rewind resp */
    case 0x28: return 1;   /* WR_REQ_DATA — accept N bytes (test/host write) */
    case 0x2C: return -3;  /* WR_HOST_DATA — 1 length byte + length data bytes */
    case 0x2D: return 0;   /* WR_REQ_DATA (file write) */
    case 0x2E: return -2;  /* WR_OFS_DATA — 2 fixed bytes + length-determined tail */
    case 0x2F: return -1;  /* SET_FILE_NAME (NUL-terminated) */
    case 0x30: return 0;   /* DISK_CONNECT */
    case 0x31: return 0;   /* DISK_MOUNT */
    case 0x32: return 0;   /* FILE_OPEN */
    case 0x33: return 0;   /* FILE_ENUM_GO */
    case 0x34: return 0;   /* FILE_CREATE */
    case 0x35: return 0;   /* FILE_ERASE */
    case 0x36: return 1;   /* FILE_CLOSE — 1 byte: update flag */
    case 0x37: return 1;   /* DIR_INFO_READ — index */
    case 0x38: return 0;   /* DIR_INFO_SAVE */
    case 0x39: return 4;   /* BYTE_LOCATE */
    case 0x3A: return 2;   /* BYTE_READ */
    case 0x3B: return 0;   /* BYTE_RD_GO */
    case 0x3C: return 2;   /* BYTE_WRITE */
    case 0x3D: return 0;   /* BYTE_WR_GO */
    case 0x3E: return 0;   /* DISK_CAPACITY */
    case 0x3F: return 0;   /* DISK_QUERY */
    case 0x45: return 1;   /* SET_ADDRESS — 1 byte */
    case 0x49: return 1;   /* SET_CONFIG  — 1 byte */
    case 0x4E: return 2;   /* ISSUE_TKN_X — token + endpoint */
    case 0x54: return 5;   /* DISK_READ  — LBA[4] + count[1] */
    case 0x55: return 0;   /* DISK_RD_GO */
    case 0x56: return 5;   /* DISK_WRITE — LBA[4] + count[1] */
    case 0x57: return 0;   /* DISK_WR_GO */
    default:   return 0;
    }
}

static void execute(CH376 *ch) {
    u8 cmd = ch->pending_cmd;
    u8 *p  = ch->params;

    if (ch376_trace) {
        fprintf(stderr, "[albireo] cmd %02X %-14s", cmd, cmd_name(cmd));
        for (int i = 0; i < ch->param_count && i < 16; i++)
            fprintf(stderr, " %02X", p[i]);
        if (cmd == 0x2F && ch->param_count > 0)
            fprintf(stderr, "  \"%s\"", (char *)p);
        fprintf(stderr, "\n");
    }

    switch (cmd) {

    case 0x01: /* GET_IC_VER */
        set_oneshot(ch, 0x43);  /* CH376 v2 firmware-ish */
        break;
    case 0x05: /* RESET_ALL */
        ch376_reset(ch);
        return;

    case 0x06: /* CHECK_EXIST */
        set_oneshot(ch, (u8)~p[0]);
        break;
    case 0x0B: /* SET_FREQ */
    case 0x13: /* SET_USB_ADDR */
        break;

    case 0x15: /* SET_USB_MODE — host reads CMD_RET_SUCCESS from DATA */
        ch->usb_mode = p[0];
        set_oneshot(ch, 0x51);
        break;

    case 0x22: /* GET_STATUS */
        set_oneshot(ch, ch->int_status);
        ch->int_pending = false;
        break;

    case 0x27: /* RD_USB_DATA0 — rewind; host then reads len then data */
        ch->resp_pos = 0;
        break;

    case 0x28: { /* WR_REQ_DATA (test): accept N bytes then echo nothing */
        ch->wbuf_len = p[0];
        ch->wbuf_pos = 0;
        break;
    }

    case 0x2D: { /* WR_REQ_DATA — host asks for max accept (UNIDOS file write) */
        u32 want = ch->bytes_remaining;
        if (want > CH_CHUNK) want = CH_CHUNK;
        u8 v = (u8)want;
        set_oneshot(ch, v);
        ch->wbuf_len = (int)want;
        ch->wbuf_pos = 0;
        break;
    }

    case 0x2F: { /* SET_FILE_NAME */
        ch->params[ch->param_count] = 0;
        normalise_path((char *)ch->params, ch->filename, sizeof(ch->filename));
        /* UNIDOS sends padded 8.3 names ("TCPTEST .BAS"); compact the leaf so
         * fat_open's case-insensitive lookup matches the real on-disk name. */
        char *slash = strrchr(ch->filename, '/');
        compact_8_3_leaf(slash ? slash + 1 : ch->filename);
        break;
    }

    case 0x30: /* DISK_CONNECT */
        raise_int(ch, ch->mounted ? USB_INT_SUCCESS : USB_INT_DISK_ERR);
        break;

    case 0x31: { /* DISK_MOUNT */
        if (!ch->mounted && ch->fp) {
            /* Real CH376 auto-detects partitioned vs raw-FAT disks. Check
             * for an MBR partition table at LBA 0: signature 0x55/0xAA at
             * offset 510, and a non-trivial first-partition LBA. */
            ch->partition_offset = 0;
            u8 mbr[512];
            if (fseek(ch->fp, 0, SEEK_SET) == 0 &&
                fread(mbr, 1, 512, ch->fp) == 512 &&
                mbr[510] == 0x55 && mbr[511] == 0xAA) {
                /* Partition table entry 1 is at offset 0x1BE; LBA-start at
                 * +8 (4 bytes LE). Only treat it as a partition if the
                 * type byte (+4) is a known FAT type and the start LBA is
                 * non-zero — otherwise this is a raw FAT volume that just
                 * happens to carry the boot signature. */
                u8 ptype = mbr[0x1BE + 4];
                u32 plba  = (u32)mbr[0x1BE + 8]
                          | ((u32)mbr[0x1BE + 9]  << 8)
                          | ((u32)mbr[0x1BE + 10] << 16)
                          | ((u32)mbr[0x1BE + 11] << 24);
                if (plba > 0 && (ptype == 0x01 || ptype == 0x04 ||
                                 ptype == 0x06 || ptype == 0x0B ||
                                 ptype == 0x0C || ptype == 0x0E))
                    ch->partition_offset = plba;
            }
            ch->mounted = fat_mount(&ch->vol, ch->fp);
        }
        raise_int(ch, ch->mounted ? USB_INT_SUCCESS : USB_INT_DISK_ERR);
        break;
    }

    case 0x32: /* FILE_OPEN */
        raise_int(ch, do_file_open(ch));
        break;

    case 0x33: /* FILE_ENUM_GO */
        raise_int(ch, do_file_enum_go(ch));
        break;

    case 0x34: /* FILE_CREATE */
        raise_int(ch, do_file_create(ch));
        break;

    case 0x35: /* FILE_ERASE — not supported by fat.c yet */
        raise_int(ch, USB_INT_DISK_ERR);
        break;

    case 0x36: /* FILE_CLOSE */
        close_file(ch);
        raise_int(ch, USB_INT_SUCCESS);
        break;

    case 0x37: /* DIR_INFO_READ — expose 32-byte entry length-prefixed */
        if (ch->have_dir_entry) {
            serve_dir_entry(ch);
            raise_int(ch, USB_INT_SUCCESS);
        } else {
            raise_int(ch, ERR_MISS_FILE);
        }
        break;

    case 0x2E: { /* WR_OFS_DATA — patch bytes in the chip's directory-info
                  * buffer. Params: offset (1) + length (1) + length data
                  * bytes. UNIDOS uses this to rewrite the file name fields
                  * before issuing DIR_INFO_SAVE. */
        u8 offset = p[0];
        u8 length = p[1];
        if ((int)offset + (int)length > 32 ||
            ch->param_count < 2 + (int)length) {
            raise_int(ch, USB_INT_DISK_ERR);
            break;
        }
        memcpy(&ch->last_dir_entry[offset], &p[2], length);
        raise_int(ch, USB_INT_SUCCESS);
        break;
    }

    case 0x38: /* DIR_INFO_SAVE — write the 32-byte entry back to disk */
        if (ch->have_dir_entry && ch->dir_entry_writable &&
            fat_write_dir_entry(&ch->vol, ch->dir_entry_sector,
                                ch->dir_entry_offset, ch->last_dir_entry))
            raise_int(ch, USB_INT_SUCCESS);
        else
            raise_int(ch, USB_INT_DISK_ERR);
        break;

    case 0x39: { /* BYTE_LOCATE */
        u32 pos = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        if (ch->file && fat_seek(ch->file, pos))
            raise_int(ch, USB_INT_SUCCESS);
        else
            raise_int(ch, USB_INT_DISK_ERR);
        break;
    }

    case 0x3A: { /* BYTE_READ */
        u32 n = (u32)p[0] | ((u32)p[1] << 8);
        ch->bytes_remaining = n;
        ch->reading = true;
        raise_int(ch, do_byte_read(ch));
        break;
    }

    case 0x3B: /* BYTE_RD_GO */
        raise_int(ch, do_byte_read(ch));
        break;

    case 0x3C: { /* BYTE_WRITE */
        u32 n = (u32)p[0] | ((u32)p[1] << 8);
        ch->bytes_remaining = n;
        ch->writing = true;
        if (!ch->file) raise_int(ch, USB_INT_DISK_ERR);
        else           raise_int(ch, USB_INT_DISK_WRITE);
        break;
    }

    case 0x3D: /* BYTE_WR_GO — flush wbuf then signal next state */
        if (ch->writing && ch->file && ch->wbuf_len > 0) {
            fat_write(ch->file, ch->wbuf, (u32)ch->wbuf_len);
            ch->wbuf_len = 0;
        }
        if (ch->writing && ch->bytes_remaining > 0)
            raise_int(ch, USB_INT_DISK_WRITE);
        else { ch->writing = false; raise_int(ch, USB_INT_SUCCESS); }
        break;

    case 0x3E: { /* DISK_CAPACITY — length-prefixed 4-byte total-sector count */
        u32 sec = ch->mounted ? ch->vol.total_sectors : 0;
        u8 v[5] = { 4, (u8)sec, (u8)(sec >> 8), (u8)(sec >> 16), (u8)(sec >> 24) };
        set_response(ch, v, 5);
        raise_int(ch, USB_INT_SUCCESS);
        break;
    }

    case 0x3F: { /* DISK_QUERY — length-prefixed 9-byte payload:
                  *   total_sectors[4] + free_sectors[4] + fs_type[1]. */
        u32 total = 0, freesec = 0;
        u8  fstype = 0;
        if (ch->mounted) {
            total   = ch->vol.total_sectors;
            freesec = fat_free_kb(&ch->vol) * 2;   /* 1 sector = 0.5 KB */
            fstype  = (ch->vol.fat_type == 32) ? 0x03
                    : (ch->vol.fat_type == 16) ? 0x02
                    : (ch->vol.fat_type == 12) ? 0x01 : 0x00;
        }
        u8 v[10] = {
            9,
            (u8)total,   (u8)(total   >>  8), (u8)(total   >> 16), (u8)(total   >> 24),
            (u8)freesec, (u8)(freesec >>  8), (u8)(freesec >> 16), (u8)(freesec >> 24),
            fstype
        };
        set_response(ch, v, 10);
        raise_int(ch, USB_INT_SUCCESS);
        break;
    }

    case 0x45: /* SET_ADDRESS — accept USB device address from host */
    case 0x49: /* SET_CONFIG  — accept configuration */
        raise_int(ch, USB_INT_SUCCESS);
        break;

    case 0x54: { /* DISK_READ — start raw sector read.
                  * Params: LBA[4] LE + count[1]. After this, host loops
                  * RD_USB_DATA0 (64 bytes) + DISK_RD_GO until status=SUCCESS. */
        if (ch376_disable_disk_read) {
            raise_int(ch, USB_INT_DISK_ERR);
            break;
        }
        u32 lba = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        u8  count = p[4];
        if (!ch->fp || count == 0) { raise_int(ch, USB_INT_DISK_ERR); break; }
        ch->sec_lba = lba;
        ch->sec_remaining = count;
        ch->sec_pos = 0;
        ch->sec_reading = true;
        if (fseek(ch->fp, (long)(lba + ch->partition_offset) * 512, SEEK_SET) != 0 ||
            fread(ch->sec_buf, 1, 512, ch->fp) == 0) {
            ch->sec_reading = false;
            raise_int(ch, USB_INT_DISK_ERR);
            break;
        }
        /* Stage first 64-byte chunk. */
        ch->resp[0] = 64;
        memcpy(&ch->resp[1], &ch->sec_buf[0], 64);
        ch->resp_len = 65;
        ch->resp_pos = 0;
        ch->sec_pos = 64;
        raise_int(ch, USB_INT_DISK_READ);
        break;
    }

    case 0x55: { /* DISK_RD_GO — next 64-byte chunk */
        if (!ch->sec_reading) { raise_int(ch, USB_INT_DISK_ERR); break; }
        if (ch->sec_pos >= 512) {
            /* Sector consumed — advance. */
            ch->sec_remaining--;
            if (ch->sec_remaining == 0) {
                ch->sec_reading = false;
                raise_int(ch, USB_INT_SUCCESS);
                break;
            }
            ch->sec_lba++;
            if (fseek(ch->fp, (long)(ch->sec_lba + ch->partition_offset) * 512, SEEK_SET) != 0 ||
                fread(ch->sec_buf, 1, 512, ch->fp) == 0) {
                ch->sec_reading = false;
                raise_int(ch, USB_INT_DISK_ERR);
                break;
            }
            ch->sec_pos = 0;
        }
        int chunk = (ch->sec_pos + 64 <= 512) ? 64 : (512 - ch->sec_pos);
        ch->resp[0] = (u8)chunk;
        memcpy(&ch->resp[1], &ch->sec_buf[ch->sec_pos], chunk);
        ch->resp_len = chunk + 1;
        ch->resp_pos = 0;
        ch->sec_pos += chunk;
        raise_int(ch, USB_INT_DISK_READ);
        break;
    }

    case 0x56: { /* DISK_WRITE — start raw sector write.
                  * Params: LBA[4] + count[1]. Host then loops WR_REQ_DATA
                  * (asks how many bytes accepted) + data writes + DISK_WR_GO
                  * to commit chunks. */
        u32 lba = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        u8  count = p[4];
        if (!ch->fp || count == 0) { raise_int(ch, USB_INT_DISK_ERR); break; }
        ch->sec_lba = lba;
        ch->sec_remaining = count;
        ch->sec_pos = 0;
        ch->sec_writing = true;
        memset(ch->sec_buf, 0, 512);
        raise_int(ch, USB_INT_DISK_WRITE);
        break;
    }

    case 0x2C: { /* WR_HOST_DATA — push N bytes into the chip's host TX buffer.
                  * SymbOS uses this as the data-deposit step for raw sector
                  * writes; the bytes accumulate into sec_buf at sec_pos. */
        u8 length = p[0];
        if (ch->sec_writing) {
            int take = (int)length;
            if (ch->sec_pos + take > 512) take = 512 - ch->sec_pos;
            if (take > 0) {
                memcpy(&ch->sec_buf[ch->sec_pos], &p[1], take);
                ch->sec_pos += take;
            }
        }
        raise_int(ch, USB_INT_SUCCESS);
        break;
    }

    case 0x57: /* DISK_WR_GO — commit when sec_buf is full, request next chunk */
        if (!ch->sec_writing) { raise_int(ch, USB_INT_DISK_ERR); break; }
        if (ch->sec_pos >= 512) {
            if (fseek(ch->fp, (long)(ch->sec_lba + ch->partition_offset) * 512, SEEK_SET) != 0 ||
                fwrite(ch->sec_buf, 1, 512, ch->fp) != 512) {
                ch->sec_writing = false;
                raise_int(ch, USB_INT_DISK_ERR);
                break;
            }
            fflush(ch->fp);
            ch->sec_remaining--;
            ch->sec_lba++;
            ch->sec_pos = 0;
            memset(ch->sec_buf, 0, 512);
            if (ch->sec_remaining == 0) {
                ch->sec_writing = false;
                raise_int(ch, USB_INT_SUCCESS);
                break;
            }
        }
        raise_int(ch, USB_INT_DISK_WRITE);
        break;

    case 0x4E: { /* ISSUE_TKN_X — params: (token, endpoint+pid).
                  * SymbOS / Albireo HID mouse path uses endpoint 0x19
                  * (read endpoint 1) with token DATA0/DATA1 alternating.
                  * Stage a 3-byte boot-mouse report (buttons, dx, dy). */
        u8 endpoint = p[1];
        if ((endpoint & 0x0F) == 0x09 && (endpoint & 0xF0) == 0x10) {
            /* IN token on endpoint 1 — return mouse HID report. */
            int dx = ch->mouse_dx;
            int dy = ch->mouse_dy;
            if (dx >  127) dx =  127; else if (dx < -128) dx = -128;
            if (dy >  127) dy =  127; else if (dy < -128) dy = -128;
            ch->mouse_dx -= dx;
            ch->mouse_dy -= dy;
            u8 v[4] = { 3, ch->mouse_buttons, (u8)(s8)dx, (u8)(s8)dy };
            set_response(ch, v, 4);
            raise_int(ch, USB_INT_SUCCESS);
        } else {
            /* Anything else on the USB token path — nothing to emulate. */
            raise_int(ch, USB_INT_DISCONNECT);
        }
        break;
    }

    default:
        /* Unknown command — leave status reflecting disk error so software
         * doesn't hang on a missing interrupt. */
        raise_int(ch, USB_INT_DISK_ERR);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Bus interface                                                              */

void ch376_write(CH376 *ch, u8 reg, u8 val) {
    if (reg == 1) {
        /* Command port */
        ch->pending_cmd  = val;
        ch->param_count  = 0;
        ch->param_needed = param_count_for(val);
        if (ch->param_needed == 0) {
            execute(ch);
            clear_pending(ch);
        }
        return;
    }

    /* Data port: parameter or write-stream byte */
    if (ch->pending_cmd) {
        if (ch->param_needed == -1) {
            if (ch->param_count < (int)sizeof(ch->params) - 1)
                ch->params[ch->param_count++] = val;
            if (val == 0) {
                execute(ch);
                clear_pending(ch);
            }
        } else if (ch->param_needed == -2) {
            /* WR_OFS_DATA: 2 prefix bytes (offset, length) then `length` data.
             * Total expected = 2 + params[1] (once we've seen the second byte). */
            if (ch->param_count < (int)sizeof(ch->params))
                ch->params[ch->param_count++] = val;
            if (ch->param_count >= 2 &&
                    ch->param_count >= 2 + (int)ch->params[1]) {
                execute(ch);
                clear_pending(ch);
            }
        } else if (ch->param_needed == -3) {
            /* WR_HOST_DATA: 1 length byte then `length` data bytes. */
            if (ch->param_count < (int)sizeof(ch->params))
                ch->params[ch->param_count++] = val;
            if (ch->param_count >= 1 &&
                    ch->param_count >= 1 + (int)ch->params[0]) {
                execute(ch);
                clear_pending(ch);
            }
        } else if (ch->param_count < ch->param_needed) {
            ch->params[ch->param_count++] = val;
            if (ch->param_count >= ch->param_needed) {
                execute(ch);
                clear_pending(ch);
            }
        }
        return;
    }

    /* No command pending: this is a write-stream byte (test write, or
     * file-write payload during BYTE_WRITE/WR_REQ_DATA). */
    if (ch->wbuf_len > 0 && ch->wbuf_pos < ch->wbuf_len) {
        ch->wbuf[ch->wbuf_pos++] = val;
        if (ch->wbuf_pos >= ch->wbuf_len) {
            /* Buffer full — for file-write flow, payload is flushed on
             * BYTE_WR_GO. Update remaining count for that path. */
            if (ch->writing) {
                u32 chunk = (u32)ch->wbuf_pos;
                if (chunk > ch->bytes_remaining) chunk = ch->bytes_remaining;
                ch->bytes_remaining -= chunk;
            }
        }
    }
}

void ch376_mouse_move(CH376 *ch, int dx, int dy) {
    ch->mouse_dx += dx;
    /* Albireo / boot-protocol HID mice report Y as "positive = down".
     * SDL's relative motion is also "positive = down", so no flip needed. */
    ch->mouse_dy += dy;
}

void ch376_mouse_button(CH376 *ch, int btn, bool pressed) {
    if (btn < 0 || btn > 2) return;
    u8 mask = (u8)(1 << btn);
    if (pressed) ch->mouse_buttons |= mask;
    else         ch->mouse_buttons &= (u8)~mask;
}

u8 ch376_read(CH376 *ch, u8 reg) {
    if (reg == 1) {
        /* STATUS: bit4=BUSY (always 0), bit7=!INT (0 = active) */
        return ch->int_pending ? 0x00 : 0x80;
    }
    /* DATA port — single-byte one-shot (GET_STATUS / CHECK_EXIST / etc.)
     * is served before any chunk buffer from BYTE_READ / DIR_INFO_READ. */
    if (ch->oneshot_valid) {
        ch->oneshot_valid = false;
        return ch->oneshot;
    }
    if (ch->resp_pos < ch->resp_len)
        return ch->resp[ch->resp_pos++];
    return 0xFF;
}
