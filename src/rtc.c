#include "rtc.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rtc_init(RTC *r) {
    memset(r, 0, sizeof(*r));
    /* Register B default: 0x06 = 24-hour, binary time format.
     * HDCPM and its |TIME/|DATE RSX commands assume binary values
     * from the RTC (they binary→BCD double-dabble before display
     * and compute the year as century*100+year, which only works
     * if both registers are binary). Setting BCD-mode here makes
     * |DATE print bogus years (e.g. 3238).
     *
     * The earlier konCePCja-style "always BCD" was useful only as
     * a workaround for the #102 boot hang — the actual fix is the
     * cycle-table port in src/z80.c, so we can restore the binary
     * default that HDCPM needs. */
    r->regb = 0x06;
}

void rtc_write_addr(RTC *r, u8 addr) {
    r->addr = addr & 0x7F;
    /* DS12887 protocol: writing the address register starts a new
     * read sequence. Invalidate the cached time so the next reg-A
     * read snapshots wall-clock fresh. */
    r->cache_valid = false;
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

static struct tm rtc_now(void) {
    struct tm fake_tm;
    if (getenv("ONE_K_FAKE_RTC")) {
        memset(&fake_tm, 0, sizeof(fake_tm));
        const char *t_env = getenv("ONE_K_FAKE_RTC_TIME");
        int h = 12, m = 0, s = 0;
        if (t_env) sscanf(t_env, "%d:%d:%d", &h, &m, &s);
        fake_tm.tm_sec  = s;
        fake_tm.tm_min  = m;
        fake_tm.tm_hour = h;
        fake_tm.tm_mday = 1;
        fake_tm.tm_mon  = 0;
        fake_tm.tm_year = 124;
        fake_tm.tm_wday = 1;
        return fake_tm;
    }
    time_t t = time(NULL);
    return *localtime(&t);
}

u8 rtc_read_data(RTC *r) {
    bool binary = (r->regb >> 2) & 1;
    bool h24    = (r->regb >> 1) & 1;
    #define FMT(n) (binary ? (u8)(n) : to_bcd(n))

    /* DS12887 frozen-time cache: fill on reg-A read (HDCPM's UIP probe), then
     * serve subsequent time-register reads from the snapshot so a sequence of
     * reads (sec, min, hour, …) sees a consistent moment. Cache invalidates
     * on every address-register write. */
    bool is_reg_a = (r->addr == 0x0A);
    if (is_reg_a && !r->cache_valid) {
        struct tm snap = rtc_now();
        r->cached_sec  = snap.tm_sec;
        r->cached_min  = snap.tm_min;
        r->cached_hour = snap.tm_hour;
        r->cached_mday = snap.tm_mday;
        r->cached_mon  = snap.tm_mon;
        r->cached_year = snap.tm_year;
        r->cached_wday = snap.tm_wday;
        r->cache_valid = true;
    }
    struct tm tm_view;
    struct tm *tm = &tm_view;
    if (r->cache_valid) {
        tm_view.tm_sec  = r->cached_sec;
        tm_view.tm_min  = r->cached_min;
        tm_view.tm_hour = r->cached_hour;
        tm_view.tm_mday = r->cached_mday;
        tm_view.tm_mon  = r->cached_mon;
        tm_view.tm_year = r->cached_year;
        tm_view.tm_wday = r->cached_wday;
    } else {
        tm_view = rtc_now();
    }

    u8 result;
    switch (r->addr) {
    case 0x00: result = FMT(tm->tm_sec);  break;
    case 0x02: result = FMT(tm->tm_min);  break;
    case 0x04:
        if (h24) {
            result = FMT(tm->tm_hour);
        } else {
            int h = tm->tm_hour % 12; if (h == 0) h = 12;
            u8 pm = (tm->tm_hour >= 12) ? 0x80 : 0x00;
            result = (u8)(FMT(h) | pm);
        }
        break;
    /* Day of week: ISO with Sunday = 7 (not 1 as in C tm_wday). Match konCePCja. */
    case 0x06: { int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
                 result = FMT(wday); break; }
    case 0x07: result = FMT(tm->tm_mday); break;
    case 0x08: result = FMT(tm->tm_mon + 1); break;
    case 0x09: result = FMT(tm->tm_year % 100); break;
    /* Reg A: UIP=0, DV=010, RS=0110 = 0x26 (DS12887 default). */
    case 0x0A: result = 0x26; break;
    case 0x0B: result = r->regb; break;
    case 0x0C: result = 0x00; break;
    case 0x0D: result = 0x80; break;
    case 0x32: result = FMT((tm->tm_year + 1900) / 100); break;
    default:
        result = (r->addr >= 0x0E && r->addr <= 0x7F)
                 ? r->nvram[r->addr - 0x0E]
                 : 0xFF;
        break;
    }
    return result;
}
