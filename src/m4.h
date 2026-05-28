#pragma once
#include "types.h"
#include "mem.h"
#include "fat.h"
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>

#define M4_ROM_SLOT  6     /* expansion slot for M4ROM — official recommendation; AMSDOS stays at slot 7 */
#define M4_MAX_FDS   8     /* file descriptors, 1-indexed */
#define M4_CMD_BUF   2304  /* big enough for C_NETSEND payload (~2KB + header) */
#define M4_PATH_MAX  512
#define M4_NSOCKS    5     /* sock 0 reserved for DNS, 1..4 are user TCP sockets */

/* M4 board error codes — these go through M4ROM's ff_error_map (FatFs FRESULT
 * indices) before reaching BASIC, so the values MUST match FatFs FR_* codes.
 * The map turns 4/5/6 into AMSDOS 0x92 (file not found), 8 into 0x91 (file
 * already exists), 20 into AMSDOS 0x0E ("file already open"), and others
 * into 0xFF (generic disk error). */
#define M4_OK          0  /* FR_OK */
#define M4_ERR_IO      1  /* FR_DISK_ERR — generic I/O failure */
#define M4_ERR_NOFILE  4  /* FR_NO_FILE  — opens "file not found" path */
#define M4_ERR_NOPATH  5  /* FR_NO_PATH  — also "file not found" */
#define M4_ERR_BADNAME 6  /* FR_INVALID_NAME */
#define M4_ERR_DENIED  7  /* FR_DENIED   — permission/write-protect */
#define M4_ERR_EXIST   8  /* FR_EXIST    — "file already exists" */
#define M4_ERR_BADFD   9  /* FR_INVALID_OBJECT — stale/invalid fd */
#define M4_ERR_RDONLY  10 /* FR_WRITE_PROTECTED */
#define M4_ERR_FULL    18 /* FR_TOO_MANY_OPEN_FILES */
#define M4_ERR_EOF     20 /* M4-specific: EOF / "file already open" sentinel */
#define M4_ERR_NOTSUP  0xFF

typedef struct {
    FILE    *fp;        /* directory-mode host file */
    FatFile *fatf;      /* image-mode FAT file (NULL = host file) */
    bool     in_use;
} M4Fd;

typedef struct {
    int  fd;                 /* host POSIX socket fd, -1 = unused */
    u8   status;             /* current status byte (matches sock_info+0) */
    u8   lastcmd;            /* matches sock_info+1 */
    u16  rx_count;            /* bytes available to read (matches sock_info+2) */
    u8   peer_ip[4];         /* matches sock_info+4 */
    u16  peer_port;          /* matches sock_info+8 */
    bool connecting;         /* a non-blocking connect is in progress */
} M4Socket;

typedef struct {
    /* Bytes written to DATAPORT accumulate here until ACKPORT fires */
    u8      cmd_buf[M4_CMD_BUF];
    int     cmd_len;

    /* Host directory backing the file API (C_OPEN, C_CD, C_READDIR…). */
    char    root[M4_PATH_MAX];
    /* Current working directory within the SD card (file-API mode only) */
    char    cwd[M4_PATH_MAX];
    /* Optional raw FAT image backing the sector API (C_SDREAD/C_SDWRITE) AND
     * the file API when no host directory is configured. */
    char    image_path[M4_PATH_MAX];
    FILE   *image_fp;
    FatVol  image_vol;
    bool    image_mounted;        /* true when image_vol is a valid FAT volume */

    /* Per-fd FAT directory iterators (one slot per dir handle).
     * dir_fat is used in image mode in place of dir_dp. */
    FatDir *dir_fat;

    /* File descriptors (1-indexed: fd=1 → fds[0]) */
    M4Fd    fds[M4_MAX_FDS];

    /* Active directory listing (opened by C_DIRSETARGS, advanced by C_READDIR) */
    DIR    *dir_dp;
    char    dir_filter[64];  /* fnmatch pattern, e.g. "*.DSK" or "*" */

    bool    nmi_enabled;     /* true = trigger Z80 NMI after each command */
    /* "RAM mode" — true after a command strobe, the M4 board presents live
     * RAM (response / sock_info / cfg) on the expansion bus regardless of
     * which ROM the CPC has paged in. The CPU code re-enters "ROM mode"
     * by calling KL_ROM_SELECT 6, which we detect via the ROM-select port. */
    bool    ram_mode;
    u8      init_count;     /* tracks ROM init calls — ROM reads this via bus bypass */

    /* M4 board's own memory mapped on the expansion bus when M4ROM is paged in.
     * Reading these addresses while M4ROM is the active upper ROM returns from
     * this buffer instead of CPC RAM, so M4 responses don't corrupt screen
     * memory (CPC screen RAM is at 0xC000-0xFFFF by default).
     *   0xE800-0xF3FF: rom_response (.ds 0xC00 in M4ROM.s) — large enough
     *                  for a 2KB C_READ payload (resp+3 status, resp+4+ data
     *                  spans up to 0xF003).
     *   0xF400-0xF4FF: rom_config — jump_vec, init_count, runfile_ptr, etc. */
    u8      bus_mem[0xC00]; /* covers 0xE800-0xF3FF — full rom_response area */
    u8      cfg_mem[0x100]; /* covers 0xF400-0xF4FF — rom_config area */
    u8      sock_mem[0x50]; /* covers 0xFE00-0xFE4F — sock_info (5 × 16 bytes) */

    /* Network state — host POSIX sockets backing the M4's WiFi sockets. */
    M4Socket sockets[M4_NSOCKS];
} M4;

void m4_init(M4 *m, const char *root);
void m4_set_image(M4 *m, const char *image_path);
void m4_reset(M4 *m);
/* Called once per frame: refreshes sock_info for any in-flight TCP work
 * (non-blocking connect completion, byte counts, remote-close detection). */
void m4_tick(M4 *m);

/* Called on every write to port 0xFE00 or 0xFF00 (DATAPORT) */
void m4_dataport_write(M4 *m, u8 val);
/* Called on a read from port 0xFE00 — returns 0 (ready) */
u8   m4_dataport_read(M4 *m);
/* Called on write to port 0xFC00 (ACKPORT) — executes buffered command,
 * writes response to CPC RAM at 0xE800, returns true if NMI should fire. */
bool m4_ackport_write(M4 *m, Mem *mem);
