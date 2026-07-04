#include "fdc.h"
#include "leds.h"
#include <string.h>

/* Command byte counts (total bytes including the command byte itself).
 * Index = command code & 0x1F. 0 = unknown/invalid. */
static const int cmd_len[32] = {
    0, 0, 9, 3, 2, 9, 9, 2,   /* 00-07 */
    1, 9, 2, 1, 9, 6, 1, 3,   /* 08-0F */
    0, 0, 9, 0, 0, 0, 0, 0,   /* 10-17 */
    0, 9, 0, 0, 0, 9, 0, 0,   /* 18-1F */
};

void fdc_init(FDC *fdc, Disk *drive_a, Disk *drive_b) {
    memset(fdc, 0, sizeof(*fdc));
    fdc->drive[0] = drive_a;
    fdc->drive[1] = drive_b;
}

void fdc_reset(FDC *fdc) {
    Disk *a = fdc->drive[0];
    Disk *b = fdc->drive[1];
    memset(fdc, 0, sizeof(*fdc));
    fdc->drive[0] = a;
    fdc->drive[1] = b;
}

uint8_t fdc_read_status(FDC *fdc) {
    switch (fdc->phase) {
    case FDC_PHASE_CMD:
        /* idle or mid-command: ready to accept bytes */
        return FDC_MSR_RQM | (fdc->cmd_pos > 0 ? FDC_MSR_BUSY : 0);
    case FDC_PHASE_EXEC:
        /* First status read after a read/write command returns BUSY without
         * RQM (matches Caprice32 / real µPD765A behaviour: the FDC needs one
         * status poll worth of "settling" before signalling data ready).
         * The Amstrad CP/M 2.2 boot relies on seeing this pre-EXEC BUSY. */
        if (fdc->read_status_delay) {
            fdc->read_status_delay--;
            return FDC_MSR_BUSY;
        }
        if (!fdc->exec_write)   /* FDC → CPU */
            return FDC_MSR_RQM | FDC_MSR_DIO | FDC_MSR_NDM | FDC_MSR_BUSY;
        else                    /* CPU → FDC */
            return FDC_MSR_RQM | FDC_MSR_NDM | FDC_MSR_BUSY;
    case FDC_PHASE_RESULT:
        return FDC_MSR_RQM | FDC_MSR_DIO | FDC_MSR_BUSY;
    }
    return FDC_MSR_RQM;
}

/* ---- Result phase helpers ---- */

static void set_result(FDC *fdc, uint8_t st0, uint8_t st1, uint8_t st2,
                       uint8_t C, uint8_t H, uint8_t R, uint8_t N) {
    fdc->result[0] = st0;
    fdc->result[1] = st1;
    fdc->result[2] = st2;
    fdc->result[3] = C;
    fdc->result[4] = H;
    fdc->result[5] = R;
    fdc->result[6] = N;
    fdc->result_len = 7;
    fdc->result_pos = 0;
    fdc->phase      = FDC_PHASE_RESULT;
}

static void set_invalid(FDC *fdc) {
    fdc->result[0] = FDC_ST0_IC_INV;
    fdc->result_len = 1;
    fdc->result_pos = 0;
    fdc->phase      = FDC_PHASE_RESULT;
}

/* ---- Command execution ---- */

static void exec_cmd(FDC *fdc) {
    uint8_t code = fdc->cmd[0] & 0x1F;
    uint8_t mfm  = (fdc->cmd[0] >> 6) & 1;  /* MFM flag */
    (void)mfm;

    switch (code) {

    /* SPECIFY — just accept, no result */
    case 0x03:
        fdc->phase = FDC_PHASE_CMD;
        break;

    /* SENSE DRIVE STATUS */
    case 0x04: {
        int drv = fdc->cmd[1] & 0x01;
        uint8_t st3 = (uint8_t)(drv & 0x01);  /* drive select */
        Disk *d = fdc->drive[drv];
        if (d && d->inserted) st3 |= 0x20;    /* ready */
        if (d && d->write_protected) st3 |= 0x40;
        if (d && d->cur_track == 0) st3 |= 0x10; /* track 0 */
        fdc->result[0] = st3;
        fdc->result_len = 1;
        fdc->result_pos = 0;
        fdc->phase = FDC_PHASE_RESULT;
        break;
    }

    /* RECALIBRATE — seek to track 0 */
    case 0x07: {
        int drv = fdc->cmd[1] & 0x01;
        Disk *d = fdc->drive[drv];
        if (d) d->cur_track = 0;
        fdc->last_st0  = FDC_ST0_SE | (uint8_t)(drv & 0x01);
        fdc->last_pcn  = 0;
        fdc->seek_done = true;
        fdc->phase     = FDC_PHASE_CMD;
        break;
    }

    /* SENSE INTERRUPT STATUS */
    case 0x08:
        if (fdc->seek_done) {
            fdc->result[0] = fdc->last_st0;
            fdc->result[1] = fdc->last_pcn;
            fdc->seek_done = false;
        } else {
            fdc->result[0] = FDC_ST0_IC_INV;
            fdc->result[1] = 0;
        }
        fdc->result_len = 2;
        fdc->result_pos = 0;
        fdc->phase      = FDC_PHASE_RESULT;
        break;

    /* SEEK */
    case 0x0F: {
        int drv  = fdc->cmd[1] & 0x01;
        int cyl  = fdc->cmd[2];
        Disk *d  = fdc->drive[drv];
        if (d) d->cur_track = cyl;
        fdc->last_st0  = FDC_ST0_SE | (uint8_t)(drv & 0x01);
        fdc->last_pcn  = (uint8_t)cyl;
        fdc->seek_done = true;
        fdc->phase     = FDC_PHASE_CMD;
        break;
    }

    /* READ DATA (0x06) / READ DELETED DATA (0x0C) */
    case 0x06:
    case 0x0C: {
        int      drv  = fdc->cmd[1] & 0x01;
        leds_ping(drv ? LED_FDC_B : LED_FDC_A);
        int      side = (fdc->cmd[1] >> 2) & 0x01;
        uint8_t  C    = fdc->cmd[2];
        uint8_t  H    = fdc->cmd[3];
        uint8_t  R    = fdc->cmd[4];
        uint8_t  N    = fdc->cmd[5];
        uint8_t  EOT  = fdc->cmd[6];
        Disk    *d    = fdc->drive[drv];
        if (!d || !d->inserted) {
            set_result(fdc, FDC_ST0_IC_AT | FDC_ST0_NR | (uint8_t)drv,
                       0, 0, C, H, R, N);
            break;
        }

        /* Read consecutive sectors R..EOT into exec_buf */
        fdc->exec_len  = 0;
        fdc->exec_pos  = 0;
        fdc->exec_write = false;

        uint8_t cur_R  = R;
        uint8_t last_C = C, last_H = H, last_R = R, last_N = N;
        uint8_t st1 = 0, st2 = 0;
        bool found_any = false;

        while (cur_R <= EOT) {
            DiskSector *sec = disk_find_sector(d, side, C, H, cur_R, N);
            if (!sec) {
                /* Try again ignoring N — some disks lie about sector size in
                 * the command. Passing N here means "same N as commanded";
                 * disk_find_sector treats it as a wildcard at the layer
                 * below when the first lookup already failed on N. */
                sec = disk_find_sector(d, side, C, H, cur_R, N);
                if (!sec) { st1 |= FDC_ST1_ND; break; }
            }
            DiskTrack *tr = &d->track[d->cur_track][side];
            int copy = sec->size;
            if (fdc->exec_len + copy > FDC_EXEC_BUF) copy = FDC_EXEC_BUF - fdc->exec_len;
            if (tr->data && sec->offset + copy <= tr->data_size)
                memcpy(fdc->exec_buf + fdc->exec_len, tr->data + sec->offset, copy);
            else
                memset(fdc->exec_buf + fdc->exec_len, 0xE5, copy);
            fdc->exec_len += copy;
            last_C = sec->C; last_H = sec->H; last_R = sec->R; last_N = sec->N;
            st1 |= sec->st1;
            st2 |= sec->st2;
            found_any = true;
            cur_R++;
        }

        if (!found_any && (st1 & FDC_ST1_ND)) {
            set_result(fdc, FDC_ST0_IC_AT | (uint8_t)drv, st1, st2, C, H, R, N);
        } else {
            /* advance R past EOT */
            last_R = EOT + 1;
            if (last_R > EOT) {
                st1 |= FDC_ST1_EN;
                last_R = EOT;
            }
            fdc->phase = FDC_PHASE_EXEC;
            fdc->read_status_delay = 1;
            /* store result for after exec. Match Caprice32 / µPD765 behavior:
             * when a READ DATA terminates at EOT (single-sector or multi-sector),
             * ST0.AT (0x40) is also set along with ST1.EN. Many CPC boot loaders
             * (including Amstrad CP/M 2.2's |cpm) test ST0 bits 6:7 (interrupt
             * code field) to distinguish "more sectors to read" from "end of
             * track reached". */
            fdc->result[0] = (uint8_t)(FDC_ST0_IC_AT | drv);
            fdc->result[1] = st1;
            fdc->result[2] = st2;
            fdc->result[3] = last_C;
            fdc->result[4] = last_H;
            fdc->result[5] = last_R;
            fdc->result[6] = last_N;
            fdc->result_len = 7;
            fdc->result_pos = 0;
        }
        break;
    }

    /* READ SECTOR ID */
    case 0x0A: {
        int drv  = fdc->cmd[1] & 0x01;
        int side = (fdc->cmd[1] >> 2) & 0x01;
        Disk *d  = fdc->drive[drv];

        if (!d || !d->inserted || d->cur_track >= d->track_count) {
            set_result(fdc, FDC_ST0_IC_AT | FDC_ST0_NR | (uint8_t)drv,
                       FDC_ST1_MA, 0, 0, 0, 0, 0);
            break;
        }
        DiskTrack *tr = &d->track[d->cur_track][side];
        if (tr->sector_count == 0) {
            set_result(fdc, FDC_ST0_IC_AT | (uint8_t)drv, FDC_ST1_MA, 0, 0, 0, 0, 0);
            break;
        }
        d->cur_sector = (d->cur_sector + 1) % tr->sector_count;
        DiskSector *s = &tr->sectors[d->cur_sector];
        set_result(fdc, (uint8_t)(FDC_ST0_IC_OK | drv), 0, 0, s->C, s->H, s->R, s->N);
        break;
    }

    /* WRITE DATA (0x05) / WRITE DELETED DATA (0x09) */
    case 0x05:
    case 0x09: {
        int drv  = fdc->cmd[1] & 0x01;
        leds_ping(drv ? LED_FDC_B : LED_FDC_A);
        int side = (fdc->cmd[1] >> 2) & 0x01;
        uint8_t C = fdc->cmd[2], H = fdc->cmd[3];
        uint8_t R = fdc->cmd[4], N = fdc->cmd[5];
        uint8_t EOT = fdc->cmd[6];
        Disk   *d  = fdc->drive[drv];

        if (!d || !d->inserted) {
            set_result(fdc, FDC_ST0_IC_AT | FDC_ST0_NR | (uint8_t)drv, 0, 0, C, H, R, N);
            break;
        }
        if (d->write_protected) {
            set_result(fdc, FDC_ST0_IC_AT | (uint8_t)drv, FDC_ST1_NW, 0, C, H, R, N);
            break;
        }

        /* Calculate how many bytes the CPU will send */
        int total = 0;
        uint8_t cur_R = R;
        while (cur_R <= EOT) {
            DiskSector *sec = disk_find_sector(d, side, C, H, cur_R, N);
            if (!sec) break;
            total += sec->size;
            cur_R++;
        }
        fdc->exec_len   = total > FDC_EXEC_BUF ? FDC_EXEC_BUF : total;
        fdc->exec_pos   = 0;
        fdc->exec_write = true;
        /* store write parameters in result for commit after exec */
        fdc->result[0] = (uint8_t)drv;
        fdc->result[1] = (uint8_t)side;
        fdc->result[2] = C; fdc->result[3] = H;
        fdc->result[4] = R; fdc->result[5] = N; fdc->result[6] = EOT;
        fdc->result_len = 7;
        fdc->result_pos = 0;
        fdc->phase = FDC_PHASE_EXEC;
        break;
    }

    /* FORMAT TRACK */
    case 0x0D: {
        /* Accept the CPU's format data then produce a dummy result */
        int drv = fdc->cmd[1] & 0x01;
        uint8_t N   = fdc->cmd[2];
        uint8_t sc  = fdc->cmd[3];   /* sectors per track */
        int total = (128 << N) * sc;
        fdc->exec_len   = total > FDC_EXEC_BUF ? FDC_EXEC_BUF : total;
        fdc->exec_pos   = 0;
        fdc->exec_write = true;
        fdc->result[0] = (uint8_t)drv;
        fdc->result_len = 7;
        fdc->result_pos = 0;
        fdc->phase = FDC_PHASE_EXEC;
        break;
    }

    default:
        set_invalid(fdc);
        break;
    }
}

/* ---- Called after EXEC phase completes (write path: commit data) ---- */

static void commit_write(FDC *fdc) {
    uint8_t code = fdc->cmd[0] & 0x1F;
    if (code != 0x05 && code != 0x09) {
        /* FORMAT or unknown — just return OK result */
        int drv = fdc->result[0] & 0x01;
        uint8_t C = fdc->cmd[2], H = fdc->cmd[3];
        uint8_t R = fdc->cmd[4], N = fdc->cmd[5];
        set_result(fdc, (uint8_t)(FDC_ST0_IC_OK | drv), 0, 0, C, H, R, N);
        return;
    }

    int      drv  = fdc->result[0] & 0x01;
    int      side = fdc->result[1] & 0x01;
    uint8_t  C    = fdc->result[2], H = fdc->result[3];
    uint8_t  R    = fdc->result[4], N = fdc->result[5];
    uint8_t  EOT  = fdc->result[6];
    Disk    *d    = fdc->drive[drv];

    if (!d || !d->inserted || d->write_protected) {
        set_result(fdc, FDC_ST0_IC_AT | FDC_ST0_NR | (uint8_t)drv, FDC_ST1_NW, 0, C, H, R, N);
        return;
    }

    int buf_off = 0;
    uint8_t cur_R = R;
    while (cur_R <= EOT && buf_off < fdc->exec_len) {
        DiskSector *sec = disk_find_sector(d, side, C, H, cur_R, N);
        if (!sec) break;
        DiskTrack *tr = &d->track[d->cur_track][side];
        int copy = sec->size;
        if (buf_off + copy > fdc->exec_len) copy = fdc->exec_len - buf_off;
        if (tr->data && sec->offset + copy <= tr->data_size) {
            if (disk_write_sector(d, sec, fdc->exec_buf + buf_off, copy) < 0) {
                set_result(fdc, FDC_ST0_IC_AT | (uint8_t)drv, FDC_ST1_NW, 0,
                           C, H, cur_R, N);
                return;
            }
            memcpy(tr->data + sec->offset, fdc->exec_buf + buf_off, copy);
        }
        buf_off += copy;
        cur_R++;
    }
    set_result(fdc, (uint8_t)(FDC_ST0_IC_OK | drv), 0, 0, C, H, EOT, N);
}

/* ---- Public read/write data register ---- */

uint8_t fdc_read_data(FDC *fdc) {
    if (fdc->phase == FDC_PHASE_EXEC && !fdc->exec_write) {
        uint8_t val = (fdc->exec_pos < fdc->exec_len)
                    ? fdc->exec_buf[fdc->exec_pos++]
                    : 0xFF;
        if (fdc->exec_pos >= fdc->exec_len) {
            /* switch to result phase */
            fdc->phase = FDC_PHASE_RESULT;
        }
        return val;
    }
    if (fdc->phase == FDC_PHASE_RESULT) {
        uint8_t val = (fdc->result_pos < fdc->result_len)
                    ? fdc->result[fdc->result_pos++]
                    : 0xFF;
        if (fdc->result_pos >= fdc->result_len)
            fdc->phase = FDC_PHASE_CMD;
        return val;
    }
    return 0xFF;
}

void fdc_write_data(FDC *fdc, uint8_t val) {
    if (fdc->phase == FDC_PHASE_EXEC && fdc->exec_write) {
        if (fdc->exec_pos < fdc->exec_len)
            fdc->exec_buf[fdc->exec_pos++] = val;
        if (fdc->exec_pos >= fdc->exec_len)
            commit_write(fdc);
        return;
    }

    if (fdc->phase != FDC_PHASE_CMD) return;

    if (fdc->cmd_pos == 0) {
        /* First byte — determine command length */
        int cl = cmd_len[val & 0x1F];
        fdc->cmd_len = (cl > 0) ? cl : 1;
        fdc->cmd_pos = 0;
    }
    if (fdc->cmd_pos < 9)
        fdc->cmd[fdc->cmd_pos] = val;
    fdc->cmd_pos++;

    if (fdc->cmd_pos >= fdc->cmd_len) {
        fdc->cmd_pos = 0;
        exec_cmd(fdc);
    }
}

void fdc_motor_write(FDC *fdc, uint8_t val) {
    fdc->motor = (val & 0x01) != 0;
}
