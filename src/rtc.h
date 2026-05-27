#pragma once
#include "types.h"

/* DS12887 Real-Time Clock — minimal emulation for the Cyboard/Symbiface II.
 *
 * Port map (CPC I/O):
 *   0xFD15  write  — address register (selects DS12887 register 0x00-0x7F)
 *   0xFD14  write  — data write to selected register
 *   0xFD14  read   — data read from selected register
 *
 * Time data comes from the host OS via localtime().  Register B controls
 * whether values are returned in BCD (default) or binary, and whether hours
 * use 12- or 24-hour format.
 */

typedef struct {
    u8 addr;        /* currently selected register */
    u8 regb;        /* register B: bit2=DM(0=BCD,1=binary), bit1=24h(0=12h,1=24h) */
    u8 nvram[114];  /* 114 bytes of battery-backed NVRAM (registers 0x0E–0x7F) */
} RTC;

void rtc_init(RTC *r);
void rtc_write_addr(RTC *r, u8 addr);
void rtc_write_data(RTC *r, u8 val);
u8   rtc_read_data(RTC *r);
