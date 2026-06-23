#include "printer.h"
#include "leds.h"

#if HAVE_CAIRO
#include <cairo.h>
#include <cairo-pdf.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>
#endif

static bool printer_capture_active(const Printer *p) {
    return p->pdf_enabled || p->sink == PRINT_SINK_REAL;
}

/* Hand the finalised PDF to CUPS via `lp`. Detaches the child so the
 * emulator doesn't stutter while CUPS queues; when the PDF is ephemeral
 * (no user-visible file), we wait for lp and unlink afterwards. */
static void printer_spool_to_lp(const char *pdf_path, bool wait_then_unlink) {
#ifdef _WIN32
    (void)pdf_path; (void)wait_then_unlink;
    fprintf(stderr, "printer: real-printer sink not implemented on Windows yet\n");
#else
    if (!pdf_path || !pdf_path[0]) return;
    pid_t pid = fork();
    if (pid < 0) { perror("printer: fork"); return; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execlp("lp", "lp", pdf_path, (char *)NULL);
        _exit(127);
    }
    if (wait_then_unlink) {
        int status;
        waitpid(pid, &status, 0);
        unlink(pdf_path);
    } else {
        static bool signal_set = false;
        if (!signal_set) { signal(SIGCHLD, SIG_IGN); signal_set = true; }
    }
#endif
}

/* US Letter at 72 dpi PDF points. The CPC printer port is just a byte
 * pipe — no physical paper geometry is implied — so we pick a sensible
 * page size for captured text. */
#define PAGE_W_PT       612.0f
#define PAGE_H_PT       792.0f
#define TEXT_MARGIN_PT   36.0f
#define TEXT_TOP_PT      54.0f
#define TEXT_FONT_PT     10.0f
#define TEXT_LINE_PT     12.0f
#define TEXT_CHAR_PT      6.0f

#define IDLE_FRAMES_TO_FINALISE 100   /* ~2 s at 50 Hz */

static void printer_reset_text(Printer *p) {
    p->text_x = TEXT_MARGIN_PT;
    p->text_y = TEXT_TOP_PT;
    p->text_esc_skip = 0;
}

void printer_init(Printer *p) {
    memset(p, 0, sizeof(*p));
    p->connected = true;
    printer_reset_text(p);
}

void printer_set_connected(Printer *p, bool connected) {
    if (p->connected == connected) return;
    if (!connected) printer_shutdown(p);
    p->connected = connected;
}

static void printer_make_pdf_path(Printer *p) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char base[64];
    if (!lt || !strftime(base, sizeof(base), "1984-print-%Y%m%d-%H%M%S", lt))
        snprintf(base, sizeof(base), "1984-print");

    p->pdf_ephemeral = !p->pdf_enabled;
    const char *tmp_env = p->pdf_ephemeral ? getenv("TMPDIR") : NULL;
    const char *dir = p->pdf_ephemeral ? (tmp_env && tmp_env[0] ? tmp_env : "/tmp")
                                       : (p->pdf_output_dir[0] ? p->pdf_output_dir : ".");
    size_t dir_len = strlen(dir);
    const char *sep = (dir_len > 0 && (dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\'))
                    ? "" : "/";
    for (int n = 0; n < 100; n++) {
        if (n == 0)
            snprintf(p->pdf_path, sizeof(p->pdf_path), "%s%s%s.pdf", dir, sep, base);
        else
            snprintf(p->pdf_path, sizeof(p->pdf_path), "%s%s%s-%02d.pdf", dir, sep, base, n);
        FILE *f = fopen(p->pdf_path, "rb");
        if (!f) return;
        fclose(f);
    }
    snprintf(p->pdf_path, sizeof(p->pdf_path), "%s%s%s-last.pdf", dir, sep, base);
}

#if HAVE_CAIRO
static bool printer_pdf_open(Printer *p) {
    if (!printer_capture_active(p)) return false;
    if (p->pdf_cr) return true;

    printer_make_pdf_path(p);
    p->pdf_surface = cairo_pdf_surface_create(p->pdf_path, PAGE_W_PT, PAGE_H_PT);
    cairo_status_t st = cairo_surface_status(p->pdf_surface);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "printer: PDF open failed for %s: %s\n",
                p->pdf_path, cairo_status_to_string(st));
        cairo_surface_destroy(p->pdf_surface);
        p->pdf_surface = NULL;
        p->pdf_path[0] = 0;
        return false;
    }
    p->pdf_cr = cairo_create(p->pdf_surface);
    st = cairo_status(p->pdf_cr);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "printer: PDF context failed for %s: %s\n",
                p->pdf_path, cairo_status_to_string(st));
        cairo_destroy(p->pdf_cr);
        cairo_surface_destroy(p->pdf_surface);
        p->pdf_cr = NULL;
        p->pdf_surface = NULL;
        p->pdf_path[0] = 0;
        return false;
    }
    cairo_set_source_rgb(p->pdf_cr, 0.0, 0.0, 0.0);
    return true;
}

static bool printer_pdf_ensure_page(Printer *p) {
    if (!printer_pdf_open(p)) return false;
    p->pdf_page_open = true;
    cairo_set_source_rgb(p->pdf_cr, 0.0, 0.0, 0.0);
    return true;
}

static void printer_pdf_show_page(Printer *p) {
    if (!p->pdf_cr || !p->pdf_page_open) return;
    cairo_show_page(p->pdf_cr);
    p->pdf_page_open = false;
    printer_reset_text(p);
}

void printer_shutdown(Printer *p) {
    char done_path[PATH_MAX] = {0};
    if (p->pdf_cr) {
        if (p->pdf_page_open) cairo_show_page(p->pdf_cr);
        cairo_destroy(p->pdf_cr);
        p->pdf_cr = NULL;
    }
    if (p->pdf_surface) {
        cairo_surface_finish(p->pdf_surface);
        cairo_surface_destroy(p->pdf_surface);
        p->pdf_surface = NULL;
        snprintf(done_path, sizeof(done_path), "%s", p->pdf_path);
    }
    p->pdf_page_open = false;
    p->pdf_path[0] = 0;

    if (p->sink == PRINT_SINK_REAL && done_path[0])
        printer_spool_to_lp(done_path, p->pdf_ephemeral);
    p->pdf_ephemeral = false;
}
#else  /* !HAVE_CAIRO */
static bool printer_pdf_ensure_page(Printer *p) { (void)p; return false; }
static void printer_pdf_show_page(Printer *p)   { (void)p; }
void printer_shutdown(Printer *p) {
    p->pdf_page_open = false;
    p->pdf_path[0] = 0;
}
#endif

void printer_set_sink(Printer *p, PrintSink sink) {
    p->sink = sink;
}

void printer_set_pdf_output_dir(Printer *p, const char *dir) {
    char next[PATH_MAX];
    snprintf(next, sizeof(next), "%s", (dir && dir[0]) ? dir : ".");
    if (strcmp(p->pdf_output_dir, next) == 0) return;
    printer_shutdown(p);
    snprintf(p->pdf_output_dir, sizeof(p->pdf_output_dir), "%s", next);
}

void printer_set_pdf_enabled(Printer *p, bool enabled) {
    if (p->pdf_enabled == enabled) return;
    if (!enabled) printer_shutdown(p);
    p->pdf_enabled = enabled;
}

static void printer_mark_active(Printer *p) {
    p->idle_countdown = IDLE_FRAMES_TO_FINALISE;
    leds_ping(LED_PRINTER);
}

void printer_tick(Printer *p) {
    if (!p->idle_countdown) return;
    if (--p->idle_countdown > 0) return;
#if HAVE_CAIRO
    if (p->pdf_cr || p->pdf_surface)
        printer_shutdown(p);
#endif
}

static void printer_text_linefeed(Printer *p) {
    p->text_x = TEXT_MARGIN_PT;
    p->text_y += TEXT_LINE_PT;
    if (p->text_y > PAGE_H_PT - TEXT_MARGIN_PT) {
        printer_pdf_show_page(p);
        printer_reset_text(p);
    }
}

/* Emit one Centronics data byte (already with strobe bit stripped). */
static void printer_emit(Printer *p, u8 val) {
    leds_ping(LED_PRINTER);
    if (!printer_capture_active(p)) return;

    if (p->text_esc_skip > 0) { p->text_esc_skip--; return; }

    switch (val) {
        case 0x1B: p->text_esc_skip = 1; return;
        case '\r': p->text_x = TEXT_MARGIN_PT; return;
        case '\n': printer_text_linefeed(p); return;
        case '\f': printer_pdf_show_page(p); return;
        case '\t': {
            int col = (int)((p->text_x - TEXT_MARGIN_PT) / TEXT_CHAR_PT);
            col = ((col / 8) + 1) * 8;
            p->text_x = TEXT_MARGIN_PT + (float)col * TEXT_CHAR_PT;
            return;
        }
        default: break;
    }

    if (val < 0x20 || val == 0x7F) return;
    if (!printer_pdf_ensure_page(p)) return;

#if HAVE_CAIRO
    cairo_select_font_face(p->pdf_cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(p->pdf_cr, TEXT_FONT_PT);
    cairo_move_to(p->pdf_cr, p->text_x, p->text_y);
    char s[2] = { (char)val, 0 };
    cairo_show_text(p->pdf_cr, s);
    printer_mark_active(p);
#endif

    p->text_x += TEXT_CHAR_PT;
    if (p->text_x > PAGE_W_PT - TEXT_MARGIN_PT)
        printer_text_linefeed(p);
}

void printer_out(Printer *p, u8 val) {
    if (!p->connected) return;
    /* Bit 7 of the raw Z80 byte asserts the strobe (cable LOW because
     * the connector inverts it). Emit whenever bit 7 is set; bit-7=0
     * writes are "data lines set, strobe deasserted" idle pokes that
     * the OS does at init / between chars — silent on real paper. */
    if (val & 0x80)
        printer_emit(p, val & 0x7F);
}
