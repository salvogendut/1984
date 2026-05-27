#include "rtc.h"
#include <time.h>
#include <string.h>

void rtc_init(RTC *r) {
    memset(r, 0, sizeof(*r));
}

void rtc_write_addr(RTC *r, u8 addr) {
    r->addr = addr & 0x7F;
}

void rtc_write_data(RTC *r, u8 val) {
    /* Only register B is writable (data mode and hour format).
     * Writes to time/date registers are accepted but ignored — host clock
     * is the authoritative time source. */
    if (r->addr == 0x0B)
        r->regb = val;
}

static u8 to_bcd(int n) {
    return (u8)(((n / 10) << 4) | (n % 10));
}

u8 rtc_read_data(RTC *r) {
    bool binary = (r->regb >> 2) & 1;
    bool h24    = (r->regb >> 1) & 1;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    switch (r->addr) {
    case 0x00: /* seconds */
        return binary ? (u8)tm->tm_sec : to_bcd(tm->tm_sec);
    case 0x02: /* minutes */
        return binary ? (u8)tm->tm_min : to_bcd(tm->tm_min);
    case 0x04: /* hours */
        if (h24) {
            return binary ? (u8)tm->tm_hour : to_bcd(tm->tm_hour);
        } else {
            int h  = tm->tm_hour % 12;
            if (h == 0) h = 12;
            u8 pm  = (tm->tm_hour >= 12) ? 0x80 : 0x00;
            return (u8)((binary ? (u8)h : to_bcd(h)) | pm);
        }
    case 0x06: /* day of week: 1=Sunday */
        return binary ? (u8)(tm->tm_wday + 1) : to_bcd(tm->tm_wday + 1);
    case 0x07: /* day of month */
        return binary ? (u8)tm->tm_mday : to_bcd(tm->tm_mday);
    case 0x08: /* month: 1-12 */
        return binary ? (u8)(tm->tm_mon + 1) : to_bcd(tm->tm_mon + 1);
    case 0x09: /* year within century: 00-99 */
        { int yr = tm->tm_year % 100;
          return binary ? (u8)yr : to_bcd(yr); }
    case 0x0A: /* register A: UIP=0 (no update in progress), DV=010 (32 kHz) */
        return 0x20;
    case 0x0B: /* register B */
        return r->regb;
    case 0x0C: /* register C: interrupt flags — none active */
        return 0x00;
    case 0x0D: /* register D: VRT=1 (battery good) */
        return 0x80;
    case 0x32: /* century register (DS12C887 extension) */
        { int century = (tm->tm_year + 1900) / 100;
          return binary ? (u8)century : to_bcd(century); }
    default:
        return 0xFF;
    }
}
