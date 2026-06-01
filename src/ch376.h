#pragma once
#include "types.h"
#include "fat.h"
#include <stdbool.h>
#include <stdio.h>

/* Albireo CPC expansion — CH376 USB host controller (partial emulation).
 *
 * Port map (CPC I/O):
 *   0xFE80  r/w  DATA      — command parameters / response payload
 *   0xFE81  w    COMMAND   — start a command
 *   0xFE81  r    STATUS    — bit7 = !INT, bit4 = BUSY
 *
 * Backend: a FAT16/FAT32 image file mounted via src/fat.c, exposed through
 * the CH376's built-in mass-storage / file-system command set used by
 * UNIDOS and similar ROMs. Sector-level commands are not implemented.
 *
 * Implemented commands (enough for UNIDOS open/read/write/dir):
 *   0x01 GET_IC_VER, 0x05 RESET_ALL, 0x06 CHECK_EXIST,
 *   0x15 SET_USB_MODE, 0x22 GET_STATUS, 0x27 RD_USB_DATA0,
 *   0x28 WR_REQ_DATA, 0x2F SET_FILE_NAME,
 *   0x30 DISK_CONNECT, 0x31 DISK_MOUNT,
 *   0x32 FILE_OPEN, 0x33 FILE_ENUM_GO, 0x34 FILE_CREATE,
 *   0x35 FILE_ERASE, 0x36 FILE_CLOSE,
 *   0x37 DIR_INFO_READ, 0x39 BYTE_LOCATE,
 *   0x3A BYTE_READ, 0x3B BYTE_RD_GO,
 *   0x3C BYTE_WRITE, 0x3D BYTE_WR_GO,
 *   0x3E DISK_CAPACITY, 0x3F DISK_QUERY.
 */

#define CH376_BUF_MAX  256

typedef struct {
    /* --- Command/response state --- */
    u8   pending_cmd;
    int  param_needed;     /* fixed param byte count; -1 = until NUL */
    int  param_count;
    u8   params[CH376_BUF_MAX];

    u8   resp[CH376_BUF_MAX];   /* response payload served via RD_USB_DATA0 */
    int  resp_len;
    int  resp_pos;

    u8   wbuf[CH376_BUF_MAX];   /* input buffer filled via WR_REQ_DATA */
    int  wbuf_len;
    int  wbuf_pos;

    /* Last interrupt status (read by GET_STATUS). */
    u8   int_status;
    bool int_pending;

    /* One-shot data-port byte served before the resp[] buffer.
     * Used by GET_STATUS / CHECK_EXIST / GET_IC_VER so they can return
     * their single result without overwriting a pending chunk buffer
     * left by BYTE_READ or DIR_INFO_READ. */
    u8   oneshot;
    bool oneshot_valid;

    u8   usb_mode;

    /* --- Backend --- */
    FILE  *fp;
    FatVol vol;
    bool   mounted;
    char   path[512];

    /* Working filename (set by SET_FILE_NAME, NUL-terminated, normalised). */
    char    filename[256];

    /* Currently open file, if any. */
    FatFile *file;
    bool     file_writing;        /* opened via FILE_CREATE */

    /* Pending byte transfer (BYTE_READ / BYTE_WRITE). */
    u32      bytes_remaining;
    bool     reading;
    bool     writing;

    /* Enumeration state (FILE_OPEN with wildcard, FILE_ENUM_GO). */
    FatDir  *enum_dir;
    char     enum_pattern[16];   /* 8.3 with wildcards */
    char     enum_parent[256];   /* parent path used to open dir */
    u8       last_dir_entry[32]; /* served by DIR_INFO_READ */
    bool     have_dir_entry;
    /* On-disk location of last_dir_entry — written back by DIR_INFO_SAVE
     * after the host edits it via WR_OFS_DATA (UNIDOS's rename path). */
    u32      dir_entry_sector;
    u16      dir_entry_offset;
    bool     dir_entry_writable;

    /* Sector-level USB Bulk-Only Transport (used by SymbOS — its FAT driver
     * reads/writes raw LBAs through the CH376 rather than the chip's
     * built-in FS commands UNIDOS uses). Buffer holds the current sector;
     * 64-byte sub-chunks are served via RD_USB_DATA0. */
    u8    sec_buf[512];
    u32   sec_lba;
    int   sec_remaining;     /* sectors still to transfer (read or write) */
    int   sec_pos;           /* byte offset within sec_buf */
    bool  sec_reading;
    bool  sec_writing;

    /* Partition offset: real CH376 auto-translates filesystem-LBAs into
     * disk-LBAs after DISK_MOUNT. For an image with an MBR, this is the
     * partition's start sector (read from the partition table at LBA 0).
     * For a raw FAT image with no MBR, this stays zero. */
    u32   partition_offset;

    /* Albireo USB HID mouse state — accumulated SDL deltas + button state.
     * Drained by an ISSUE_TKN_X (0x4E) with parameters (TOKEN, 0x19)
     * (read, endpoint 1, boot-protocol mouse). */
    int   mouse_dx;
    int   mouse_dy;
    u8    mouse_buttons;        /* bit0=left, bit1=right, bit2=middle */
} CH376;

extern int ch376_trace;

/* When set, DISK_READ (cmd 0x54) returns DISK_ERR so SymbOS falls back to
 * the chip's FS path via FILE_OPEN/BYTE_READ. Workaround for a rendering
 * regression where SymbOS-FAT-on-raw-DISK_READ corrupts on-screen text. */
extern int ch376_disable_disk_read;

void ch376_init(CH376 *ch);
void ch376_reset(CH376 *ch);
void ch376_open(CH376 *ch, const char *path);
void ch376_close(CH376 *ch);

u8   ch376_read(CH376 *ch, u8 reg);   /* reg: 0=DATA, 1=STATUS */
void ch376_write(CH376 *ch, u8 reg, u8 val);

/* USB HID mouse plumbing (Albireo emulates a USB mouse on endpoint 1). */
void ch376_mouse_move(CH376 *ch, int dx, int dy);
void ch376_mouse_button(CH376 *ch, int btn, bool pressed);  /* 0=L 1=R 2=M */
