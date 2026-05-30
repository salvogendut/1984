/* startup_debug.h — optional startup tracing to a file.
 *
 * Compiled in only when -DSTARTUP_DEBUG is passed. When active, SD_INIT()
 * opens "1984-startup.log" in the current directory and (on Windows) installs
 * an unhandled-exception filter; SD_LOG() appends a line and flushes
 * immediately, so the last line in the file is the last milestone reached
 * before an early exit or crash. This sidesteps any stdout/stderr redirection
 * the SDL Windows entry-point shim may perform.
 *
 * With STARTUP_DEBUG undefined, every macro compiles to nothing.
 */
#ifndef STARTUP_DEBUG_H
#define STARTUP_DEBUG_H

#ifdef STARTUP_DEBUG

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
#include "compat_win.h"   /* brings in <windows.h> */
#endif

static FILE *sd_log_fp = NULL;

static void sd_logf(const char *fmt, ...) {
    if (!sd_log_fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sd_log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', sd_log_fp);
    fflush(sd_log_fp);
}

#ifdef _WIN32
static LONG WINAPI sd_crash_filter(EXCEPTION_POINTERS *ep) {
    sd_logf("*** CRASH: exception 0x%08lx at address %p",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;   /* terminate, but log first */
}
#endif

static void sd_init(void) {
    sd_log_fp = fopen("1984-startup.log", "w");
    sd_logf("=== 1984 startup log ===");
#ifdef _WIN32
    SetUnhandledExceptionFilter(sd_crash_filter);
    sd_logf("HOME=%s", getenv("HOME") ? getenv("HOME") : "(null)");
    sd_logf("USERPROFILE=%s", getenv("USERPROFILE") ? getenv("USERPROFILE") : "(null)");
    {
        char cwd[1024];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) sd_logf("CWD=%s", cwd);
    }
#endif
}

#define SD_INIT()       sd_init()
#define SD_LOG(...)     sd_logf(__VA_ARGS__)

#else  /* !STARTUP_DEBUG */

#define SD_INIT()       ((void)0)
#define SD_LOG(...)     ((void)0)

#endif /* STARTUP_DEBUG */

#endif /* STARTUP_DEBUG_H */
