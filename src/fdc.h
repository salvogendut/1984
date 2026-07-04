#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "disk.h"

/* Main Status Register bits */
#define FDC_MSR_RQM  0x80   /* ready for data transfer */
#define FDC_MSR_DIO  0x40   /* 1=FDC→CPU, 0=CPU→FDC   */
#define FDC_MSR_NDM  0x20   /* non-DMA mode            */
#define FDC_MSR_BUSY 0x10   /* command in progress     */

/* ST0 bits */
#define FDC_ST0_IC_OK  0x00  /* normal termination */
#define FDC_ST0_IC_AT  0x40  /* abnormal termination */
#define FDC_ST0_IC_INV 0x80  /* invalid command */
#define FDC_ST0_SE     0x20  /* seek end */
#define FDC_ST0_EC     0x10  /* equipment check */
#define FDC_ST0_NR     0x08  /* not ready */

/* ST1 bits */
#define FDC_ST1_EN  0x80  /* end of cylinder */
#define FDC_ST1_DE  0x20  /* data error (CRC) */
#define FDC_ST1_OR  0x10  /* overrun */
#define FDC_ST1_ND  0x04  /* no data (sector not found) */
#define FDC_ST1_NW  0x02  /* not writable */
#define FDC_ST1_MA  0x01  /* missing address mark */

/* ST2 bits */
#define FDC_ST2_CM  0x40  /* control mark (deleted data) */
#define FDC_ST2_DD  0x20  /* data error in data field */
#define FDC_ST2_WC  0x10  /* wrong cylinder */
#define FDC_ST2_BC  0x02  /* bad cylinder */
#define FDC_ST2_MD  0x01  /* missing data address mark */

typedef enum {
    FDC_PHASE_CMD    = 0,
    FDC_PHASE_EXEC   = 1,
    FDC_PHASE_RESULT = 2,
} FdcPhase;

#define FDC_EXEC_BUF 8192

typedef struct {
    FdcPhase phase;

    uint8_t  cmd[9];
    int      cmd_len;       /* expected total command bytes */
    int      cmd_pos;       /* bytes received so far */

    uint8_t  result[7];
    int      result_len;
    int      result_pos;

    /* execution phase data buffer */
    uint8_t  exec_buf[FDC_EXEC_BUF];
    int      exec_len;
    int      exec_pos;
    bool     exec_write;    /* true = CPU writes to FDC (WRITE DATA) */

    /* state after SEEK / RECALIBRATE for SENSE INTERRUPT STATUS */
    uint8_t  last_st0;
    uint8_t  last_pcn;      /* physical cylinder number after seek */
    bool     seek_done;     /* pending result for SENSE INTERRUPT STATUS */

    bool     motor;
    int      read_status_delay;  /* first status read after data-ready returns BUSY (matches Caprice32) */
    int      result_delay_cycles; /* post-operation busy time before result bytes are ready */

    Disk    *drive[2];      /* drive[0]=A, drive[1]=B */
} FDC;

void fdc_init(FDC *fdc, Disk *drive_a, Disk *drive_b);
void fdc_reset(FDC *fdc);

uint8_t fdc_read_status(FDC *fdc);
uint8_t fdc_read_data(FDC *fdc);
void    fdc_write_data(FDC *fdc, uint8_t val);
void    fdc_motor_write(FDC *fdc, uint8_t val);
void    fdc_tick(FDC *fdc, int cycles);
