#pragma once
#include "types.h"
#include "mem.h"
#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>

#define M4_ROM_SLOT  6     /* expansion slot for M4ROM — official recommendation; AMSDOS stays at slot 7 */
#define M4_MAX_FDS   8     /* file descriptors, 1-indexed */
#define M4_CMD_BUF   512
#define M4_PATH_MAX  512

/* Error codes written to CPC RAM at 0xE800 */
#define M4_OK          0x00
#define M4_ERR_NOFILE  0x01
#define M4_ERR_EOF     0x02
#define M4_ERR_FULL    0x03
#define M4_ERR_BADFD   0x04
#define M4_ERR_IO      0x05
#define M4_ERR_NOTSUP  0xFF

typedef struct {
    FILE *fp;
    bool  in_use;
} M4Fd;

typedef struct {
    /* Bytes written to DATAPORT accumulate here until ACKPORT fires */
    u8      cmd_buf[M4_CMD_BUF];
    int     cmd_len;

    /* SD card root: host filesystem directory mapped as the SD card */
    char    root[M4_PATH_MAX];
    /* Current working directory within the SD card (always starts with '/') */
    char    cwd[M4_PATH_MAX];

    /* File descriptors (1-indexed: fd=1 → fds[0]) */
    M4Fd    fds[M4_MAX_FDS];

    /* Active directory listing (opened by C_DIRSETARGS, advanced by C_READDIR) */
    DIR    *dir_dp;
    char    dir_filter[64];  /* fnmatch pattern, e.g. "*.DSK" or "*" */

    bool    nmi_enabled;     /* true = trigger Z80 NMI after each command */
    u8      init_count;     /* tracks ROM init calls — ROM reads this via bus bypass */
} M4;

void m4_init(M4 *m, const char *root);
void m4_reset(M4 *m);

/* Called on every write to port 0xFE00 or 0xFF00 (DATAPORT) */
void m4_dataport_write(M4 *m, u8 val);
/* Called on a read from port 0xFE00 — returns 0 (ready) */
u8   m4_dataport_read(M4 *m);
/* Called on write to port 0xFC00 (ACKPORT) — executes buffered command,
 * writes response to CPC RAM at 0xE800, returns true if NMI should fire. */
bool m4_ackport_write(M4 *m, Mem *mem);
