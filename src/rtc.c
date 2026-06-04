#include "rtc.h"
#include <time.h>
#include <string.h>

void rtc_init(RTC *r) {
    memset(r, 0, sizeof(*r));
    /* Register B default: bit2=DM (1=binary), bit1=24h (1=24-hour).
     * 0x06 = binary, 24-hour.  The Symbiface software ecosystem (SymbOS and
     * the HDCPM ROM's |TIME/|DATE commands) expects the clock registers in
     * binary form — HDCPM double-dabbles each byte binary->BCD for display
     * and computes the year as century*100+year, both of which only work in
     * binary mode.  Defaulting to BCD here made |TIME/|DATE print garbage on
     * a cold boot until SymbOS reprogrammed register B to binary. */
    r->regb = 0x06; /* binary, 24-hour */
}

void rtc_write_addr(RTC *r, u8 addr) {
    r->addr = addr & 0x7F;
}

void rtc_write_data(RTC *r, u8 val) {
    if (r->addr == 0x0B) {
        r->regb = val;
    } else if (r->addr >= 0x0E && r->addr <= 0x7F) {
        r->nvram[r->addr - 0x0E] = val;
    }
    /* writes to time/date registers ignored — host clock is authoritative */
}

static u8 to_bcd(int n) {
    return (u8)(((n / 10) << 4) | (n % 10));
}

u8 rtc_read_data(RTC *r) {
    bool binary = (r->regb >> 2) & 1;
    bool h24    = (r->regb >> 1) & 1;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);

    u8 result;
    switch (r->addr) {
    case 0x00: result = binary ? (u8)tm->tm_sec  : to_bcd(tm->tm_sec);  break;
    case 0x02: result = binary ? (u8)tm->tm_min  : to_bcd(tm->tm_min);  break;
    case 0x04:
        if (h24) {
            result = binary ? (u8)tm->tm_hour : to_bcd(tm->tm_hour);
        } else {
            int h = tm->tm_hour % 12; if (h == 0) h = 12;
            u8 pm = (tm->tm_hour >= 12) ? 0x80 : 0x00;
            result = (u8)((binary ? (u8)h : to_bcd(h)) | pm);
        }
        break;
    case 0x06: result = binary ? (u8)(tm->tm_wday+1) : to_bcd(tm->tm_wday+1); break;
    case 0x07: result = binary ? (u8)tm->tm_mday : to_bcd(tm->tm_mday); break;
    case 0x08: result = binary ? (u8)(tm->tm_mon+1) : to_bcd(tm->tm_mon+1); break;
    case 0x09: { int yr = tm->tm_year % 100;
                 result = binary ? (u8)yr : to_bcd(yr); break; }
    case 0x0A: result = 0x20; break;
    case 0x0B: result = r->regb; break;
    case 0x0C: result = 0x00; break;
    case 0x0D: result = 0x80; break;
    case 0x32: { int c = (tm->tm_year + 1900) / 100;
                 result = binary ? (u8)c : to_bcd(c); break; }
    default:
        result = (r->addr >= 0x0E && r->addr <= 0x7F)
                 ? r->nvram[r->addr - 0x0E]
                 : 0xFF;
        break;
    }
    return result;
}
