#pragma once
#include "types.h"
#include <stdio.h>

/* DS12887-compatible IDE emulation for the SYMBiFACE II / Cyboard add-on.
 *
 * Port map (CPC I/O):
 *   0xFD06  r/w  Data register (16-bit ATA word served as 2 consecutive bytes)
 *   0xFD07  r    Error register   /  w  Features
 *   0xFD08  r/w  Sector Count
 *   0xFD09  r/w  LBA Low  (sector number)
 *   0xFD0A  r/w  LBA Mid  (cylinder low)
 *   0xFD0B  r/w  LBA High (cylinder high)
 *   0xFD0C  r/w  Device/Head (bits 3-0 = LBA bits 27-24)
 *   0xFD0D  r    Status  /  w  Command
 *   0xFD0E  r    Alternate Status  /  w  Device Control
 *
 * Backend: raw disk image file (FAT16/FAT32 formatted).
 * Only LBA addressing is supported.
 */

#define IDE_STATUS_BSY  0x80u
#define IDE_STATUS_DRDY 0x40u
#define IDE_STATUS_DSC  0x10u   /* drive seek complete — many drivers expect this set whenever the drive is ready */
#define IDE_STATUS_DRQ  0x08u
#define IDE_STATUS_ERR  0x01u

#define IDE_ERR_ABRT    0x04u
#define IDE_ERR_IDNF    0x10u

typedef struct {
    /* ATA task-file registers */
    u8  error;
    u8  features;
    u8  sector_count;
    u8  lba_low;
    u8  lba_mid;
    u8  lba_high;
    u8  device;
    u8  status;
    u8  control;
    u8  cmd;

    /* Sector transfer buffer */
    u8  buf[512];
    int buf_pos;

    /* Multi-sector transfer state */
    u32 current_lba;
    int sectors_left;
    bool writing;

    /* Backend */
    FILE *fp;
    u64   num_sectors;
} IDE;

void ide_init(IDE *ide);    /* full init — does not touch fp/num_sectors */
void ide_reset(IDE *ide);   /* soft reset — resets ATA state, keeps file open */
void ide_open(IDE *ide, const char *path);
void ide_close(IDE *ide);

u8   ide_read(IDE *ide, u8 reg);     /* reg = lo byte of CPC port (0x06–0x0E) */
void ide_write(IDE *ide, u8 reg, u8 val);
