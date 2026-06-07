#include "rtc.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
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

u8 rtc_read_data(RTC *r) {
    bool binary = (r->regb >> 2) & 1;
    bool h24    = (r->regb >> 1) & 1;

    /* DS12887 frozen-time cache. Refresh only when:
     *   - the cache has been invalidated (by an address-register write)
     *   - AND software is reading register A (the UIP check that
     *     starts every legitimate read sequence — see HDCPM
     *     rtcdriver.asm lines 28-34).
     * For all other reads, return the cached snapshot. This honours
     * the 244-µs stable-read window the DS12887 datasheet guarantees,
     * which HDCPM expects. */
    bool is_reg_a = (r->addr == 0x0A);
    if (is_reg_a && !r->cache_valid) {
        struct tm fake_tm;
        time_t t = time(NULL);
        struct tm *tm;
        /* Debug aid: set ONE_K_FAKE_RTC=1 to freeze the clock at a
         * deterministic instant (2024-01-01 12:00:00, Monday). */
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
            tm = &fake_tm;
        } else {
            tm = localtime(&t);
        }
        r->cached_sec  = tm->tm_sec;
        r->cached_min  = tm->tm_min;
        r->cached_hour = tm->tm_hour;
        r->cached_mday = tm->tm_mday;
        r->cached_mon  = tm->tm_mon;
        r->cached_year = tm->tm_year;
        r->cached_wday = tm->tm_wday;
        r->cache_valid = true;
    }
    /* Build a transient struct tm view of the cached snapshot for the
     * existing switch-case below. If cache isn't valid (e.g. software
     * never reads reg A first), fall back to a fresh localtime() —
     * matches our previous behaviour for non-HDCPM software. */
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
        time_t t = time(NULL);
        tm = localtime(&t);
    }

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
    /* Day of week: konCePCja uses ISO convention with Sunday = 7
     * (not 1 as in C tm_wday). Match. */
    case 0x06: { int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
                 result = binary ? (u8)wday : to_bcd(wday); break; }
    case 0x07: result = binary ? (u8)tm->tm_mday : to_bcd(tm->tm_mday); break;
    case 0x08: result = binary ? (u8)(tm->tm_mon+1) : to_bcd(tm->tm_mon+1); break;
    case 0x09: { int yr = tm->tm_year % 100;
                 result = binary ? (u8)yr : to_bcd(yr); break; }
    /* RTC Register A: bit 7 = UIP (update in progress, 0 here since we
     * always return consistent values), bits 6:4 = DV (oscillator
     * divider, =010 for 32.768 kHz crystal), bits 3:0 = RS (rate
     * select for periodic interrupt). Real DS12887 returns 0x26 by
     * default (DV=010, RS=0110 = 16 Hz periodic). konCePCja matches
     * that. We returned 0x20 (RS=0000 = no periodic), which any kernel
     * code path probing RS as an "RTC alive / reasonable" sanity check
     * would see as zero. Match konCePCja. */
    case 0x0A: result = 0x26; break;
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
