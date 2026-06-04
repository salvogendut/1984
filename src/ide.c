#define _POSIX_C_SOURCE 200112L
#define _FILE_OFFSET_BITS 64
#include "ide.h"
#include "leds.h"
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

/* ---- Helpers ---- */

static u32 lba_addr(const IDE *ide) {
    return (u32)ide->lba_low
         | ((u32)ide->lba_mid   <<  8)
         | ((u32)ide->lba_high  << 16)
         | ((u32)(ide->device & 0x0F) << 24);
}

static void set_error(IDE *ide, u8 err) {
    ide->error  = err;
    ide->status = IDE_STATUS_DRDY | IDE_STATUS_DSC | IDE_STATUS_ERR;
}

static bool load_sector(IDE *ide, u32 lba) {
    if (!ide->fp || (u64)lba >= ide->num_sectors) {
        set_error(ide, IDE_ERR_IDNF);
        return false;
    }
    if (fseeko(ide->fp, (off_t)lba * 512, SEEK_SET) != 0 ||
        fread(ide->buf, 512, 1, ide->fp) != 1) {
        set_error(ide, IDE_ERR_ABRT);
        return false;
    }
    leds_ping(LED_IDE);
    return true;
}

static bool flush_sector(IDE *ide, u32 lba) {
    if (!ide->fp || (u64)lba >= ide->num_sectors) {
        set_error(ide, IDE_ERR_IDNF);
        return false;
    }
    if (fseeko(ide->fp, (off_t)lba * 512, SEEK_SET) != 0 ||
        fwrite(ide->buf, 512, 1, ide->fp) != 1) {
        set_error(ide, IDE_ERR_ABRT);
        return false;
    }
    fflush(ide->fp);
    leds_ping(LED_IDE);
    return true;
}

/* ---- IDENTIFY DEVICE response (512 bytes, ATA word layout) ---- */

/* Write an ATA byte-swapped string into buf starting at word wstart (40-char field). */
static void ata_str(u8 *buf, int wstart, const char *s, int nwords) {
    for (int i = 0; i < nwords; i++) {
        char c0 = s[2 * i]     ? s[2 * i]     : ' ';
        char c1 = s[2 * i + 1] ? s[2 * i + 1] : ' ';
        buf[2 * (wstart + i)]     = (u8)c1;  /* ATA: high byte = 1st char */
        buf[2 * (wstart + i) + 1] = (u8)c0;
    }
}

static void setw(u8 *buf, int word, u16 val) {
    buf[2 * word]     = (u8)(val & 0xFF);
    buf[2 * word + 1] = (u8)(val >> 8);
}

static void build_identify(IDE *ide) {
    memset(ide->buf, 0, 512);

    u32 nsec = (u32)(ide->num_sectors > 0x0FFFFFFFu ? 0x0FFFFFFFu : ide->num_sectors);
    u32 cyls = nsec / (16u * 63u);
    if (cyls < 1) cyls = 1;
    if (cyls > 65535u) cyls = 65535u;

    setw(ide->buf,  0, 0x0040);               /* non-removable ATA device */
    setw(ide->buf,  1, (u16)cyls);            /* CHS cylinders */
    setw(ide->buf,  3, 16);                   /* CHS heads */
    setw(ide->buf,  6, 63);                   /* CHS sectors per track */

    ata_str(ide->buf, 10, "1984EMULATORIDE     ", 10); /* serial (words 10-19) */
    ata_str(ide->buf, 23, "1.0     ",             4);  /* firmware (words 23-26) */
    ata_str(ide->buf, 27, "1984 IDE Drive                          ", 20); /* model */

    setw(ide->buf, 49, 0x0200);               /* capabilities: LBA supported */
    setw(ide->buf, 53, 0x0001);               /* words 54-58 valid */
    setw(ide->buf, 54, (u16)cyls);
    setw(ide->buf, 55, 16);
    setw(ide->buf, 56, 63);
    u32 chs_cap = cyls * 16u * 63u;
    setw(ide->buf, 57, (u16)(chs_cap & 0xFFFFu));
    setw(ide->buf, 58, (u16)(chs_cap >> 16));
    setw(ide->buf, 60, (u16)(nsec & 0xFFFFu)); /* total LBA sectors low */
    setw(ide->buf, 61, (u16)(nsec >> 16));      /* total LBA sectors high */
    setw(ide->buf, 80, 0x0006);               /* ATA-1 / ATA-2 compatible */
}

/* ---- Command execution ---- */

static void exec_cmd(IDE *ide, u8 cmd) {
    ide->cmd     = cmd;
    ide->error   = 0;
    ide->buf_pos = 0;

    switch (cmd) {
    case 0x91:  /* INITIALIZE DRIVE PARAMETERS */
    case 0xEF:  /* SET FEATURES */
        ide->status = IDE_STATUS_DRDY | IDE_STATUS_DSC;
        break;

    case 0xEC:  /* IDENTIFY DEVICE */
        if (!ide->fp) { set_error(ide, IDE_ERR_ABRT); break; }
        build_identify(ide);
        ide->buf_pos      = 0;
        ide->writing      = false;
        ide->status       = IDE_STATUS_DRDY | IDE_STATUS_DSC | IDE_STATUS_DRQ;
        break;

    case 0x20:  /* READ SECTORS (with retry) */
    case 0x21:  /* READ SECTORS (without retry) */
        ide->current_lba  = lba_addr(ide);
        ide->sectors_left = ide->sector_count ? (int)ide->sector_count : 256;
        ide->writing      = false;
        if (load_sector(ide, ide->current_lba)) {
            ide->buf_pos = 0;
            ide->status  = IDE_STATUS_DRDY | IDE_STATUS_DSC | IDE_STATUS_DRQ;
        }
        break;

    case 0x30:  /* WRITE SECTORS (with retry) */
    case 0x31:  /* WRITE SECTORS (without retry) */
        ide->current_lba  = lba_addr(ide);
        ide->sectors_left = ide->sector_count ? (int)ide->sector_count : 256;
        ide->writing      = true;
        ide->buf_pos      = 0;
        ide->status       = IDE_STATUS_DRDY | IDE_STATUS_DSC | IDE_STATUS_DRQ;
        break;

    default:
        set_error(ide, IDE_ERR_ABRT);
        break;
    }
}

/* ---- Public API ---- */

void ide_init(IDE *ide) {
    memset(ide, 0, sizeof(*ide));
    /* fp and num_sectors intentionally zeroed — caller must call ide_open */
}

void ide_reset(IDE *ide) {
    /* Soft reset: preserve open file, reset ATA task-file */
    FILE *fp  = ide->fp;
    u64   ns  = ide->num_sectors;
    memset(ide, 0, sizeof(*ide));
    ide->fp          = fp;
    ide->num_sectors = ns;
    ide->status      = fp ? IDE_STATUS_DRDY | IDE_STATUS_DSC : 0x00;
}

void ide_open(IDE *ide, const char *path) {
    ide_close(ide);
    ide->fp = fopen(path, "r+b");
    if (!ide->fp) {
        fprintf(stderr, "1984: IDE: cannot open image '%s'\n", path);
        ide->status = 0x00;  /* drive not ready */
        return;
    }
    fseeko(ide->fp, 0, SEEK_END);
    off_t sz = ftello(ide->fp);
    ide->num_sectors = (u64)(sz > 0 ? sz : 0) / 512;
    ide->status      = IDE_STATUS_DRDY | IDE_STATUS_DSC;
}

void ide_close(IDE *ide) {
    if (ide->fp) {
        fclose(ide->fp);
        ide->fp = NULL;
    }
    ide->num_sectors = 0;
    ide->status      = 0x00;
}

u8 ide_read(IDE *ide, u8 reg) {
    switch (reg) {
    case 0x06:  return ide->status;  /* alternate status — no side effects */
    case 0x08:  /* data */
        if (!(ide->status & IDE_STATUS_DRQ) || ide->writing) return 0xFF;
        if (ide->buf_pos >= 512) return 0xFF;
        {
            u8 val = ide->buf[ide->buf_pos++];
            if (ide->buf_pos == 512) {
                if (ide->cmd == 0xEC) {
                    ide->status = IDE_STATUS_DRDY | IDE_STATUS_DSC;
                } else {
                    ide->current_lba++;
                    ide->sectors_left--;
                    if (ide->sectors_left > 0 && load_sector(ide, ide->current_lba)) {
                        ide->buf_pos = 0;
                    } else {
                        ide->status = IDE_STATUS_DRDY | IDE_STATUS_DSC;
                    }
                }
            }
            return val;
        }
    case 0x09:  return ide->error;
    case 0x0A:  return ide->sector_count;
    case 0x0B:  return ide->lba_low;
    case 0x0C:  return ide->lba_mid;
    case 0x0D:  return ide->lba_high;
    case 0x0E:  return ide->device;
    case 0x0F:  return ide->status;
    default:    return 0xFF;
    }
}

void ide_write(IDE *ide, u8 reg, u8 val) {
    switch (reg) {
    case 0x06:  /* device control */
        ide->control = val;
        if (val & 0x04) ide_reset(ide);  /* SRST */
        break;
    case 0x08:  /* data */
        if (!(ide->status & IDE_STATUS_DRQ) || !ide->writing) return;
        if (ide->buf_pos < 512) {
            ide->buf[ide->buf_pos++] = val;
            if (ide->buf_pos == 512) {
                flush_sector(ide, ide->current_lba);
                ide->current_lba++;
                ide->sectors_left--;
                if (ide->sectors_left > 0) {
                    ide->buf_pos = 0;
                    /* DRQ remains set; host writes next sector */
                } else {
                    ide->status = IDE_STATUS_DRDY | IDE_STATUS_DSC;
                }
            }
        }
        break;
    case 0x09:  ide->features     = val; break;
    case 0x0A:  ide->sector_count = val; break;
    case 0x0B:  ide->lba_low      = val; break;
    case 0x0C:  ide->lba_mid      = val; break;
    case 0x0D:  ide->lba_high     = val; break;
    case 0x0E:  ide->device       = val; break;
    case 0x0F:
        if (ide->fp) exec_cmd(ide, val);
        else set_error(ide, IDE_ERR_ABRT);
        break;
    default:
        break;
    }
}
