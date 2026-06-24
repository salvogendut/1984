#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <libgen.h>
#include <strings.h>     /* strcasecmp */
#include "config.h"
#include "overlay.h"
#include "host_mount.h"
#include "cpc.h"
#include "mem.h"
#include "paste.h"
#include "kbd_pty.h"
#include "symbols.h"
#include "screen_text.h"
#include "joy.h"
#include "net4cpc.h"
#include "n4c_stack.h"
#include "tap.h"
#include <unistd.h>
#include "monitor.h"
#include "snapshot.h"
#include "leds.h"
#include "shutter_wav.h"
#include "compat_win.h"   /* net_compat_init() — WSAStartup on Windows */
#include "startup_debug.h"   /* SD_INIT()/SD_LOG() — no-ops unless -DSTARTUP_DEBUG */
#include "gifcap.h"
#include "webmcap.h"
#include "symbos_trace.h"

/* --- Video capture state. F6 → GIF (lean, no deps); overlay file
 * picker → WebM via ffmpeg when configure detected it. Dispatch is by
 * file extension. Output is scaled to 768x576 (4:3) in both paths. */
static GifCap  *g_videocap_gif  = NULL;
static WebmCap *g_videocap_webm = NULL;
static int      g_videocap_skip = 0;   /* GIF-only: halve to 25 fps */

bool videocap_active(void) {
    return g_videocap_gif != NULL || g_videocap_webm != NULL;
}
int videocap_frame_count(void) {
    if (g_videocap_gif)  return gifcap_frame_count(g_videocap_gif);
    if (g_videocap_webm) return webmcap_frame_count(g_videocap_webm);
    return 0;
}

static bool path_ends_with(const char *p, const char *suffix) {
    size_t lp = strlen(p), ls = strlen(suffix);
    if (lp < ls) return false;
    return strcasecmp(p + lp - ls, suffix) == 0;
}

bool videocap_start(const char *path) {
    if (!path || !path[0]) return false;
    if (videocap_active()) return true;
    if (path_ends_with(path, ".webm")) {
        g_videocap_webm = webmcap_open(path,
                                       CPC_SCREEN_W, CPC_SCREEN_H,
                                       CPC_SCREEN_W, WINDOW_H);
        if (!g_videocap_webm) {
            fprintf(stderr, "[videocap] WebM start failed for '%s'\n", path);
            return false;
        }
    } else {
        /* 4 cs = 25 fps; decimate 50 Hz input by writing every other frame.
         * Output at WINDOW_H for the correct 4:3 aspect — the CPC
         * framebuffer is 768x272 non-square pixels, displayed at
         * 768x576. */
        g_videocap_gif = gifcap_open(path,
                                     CPC_SCREEN_W, CPC_SCREEN_H,
                                     CPC_SCREEN_W, WINDOW_H,
                                     4);
        if (!g_videocap_gif) {
            fprintf(stderr, "[videocap] GIF open failed for '%s'\n", path);
            return false;
        }
        g_videocap_skip = 0;
    }
    fprintf(stderr, "[videocap] recording to %s\n", path);
    return true;
}
void videocap_stop(void) {
    if (g_videocap_gif) {
        int n = gifcap_frame_count(g_videocap_gif);
        gifcap_close(g_videocap_gif);
        g_videocap_gif = NULL;
        fprintf(stderr, "[videocap] GIF stopped (%d frames)\n", n);
    }
    if (g_videocap_webm) {
        webmcap_close(g_videocap_webm);
        g_videocap_webm = NULL;
    }
}

/* Synchronise the Net4CPC TAP backend with the current config. Used at
 * boot and after the overlay flips net4cpc / net4cpc_tap. cli_tap_dev
 * is the --tap=NAME the user passed on the command line (NULL/empty if
 * none); it suppresses auto-setup so power users keep full control.
 *
 * Persistence model: the auto-tap device ('cpc-tap0' on Linux, 'tap0'
 * on BSDs — the BSD tap driver only accepts 'tap<N>' names) lives for
 * the host uptime — created once on first enable (one elevation
 * prompt), reused across subsequent 1984 launches in the same session
 * (zero prompts), cleared by the kernel on reboot. On overlay-toggle-off
 * we just detach our fd from it; the device itself stays, so flipping
 * the toggle back on also doesn't prompt. */
static void net4cpc_tap_sync(Config *cfg, const char *cli_tap_dev) {
#if !TAP_SUPPORTED
    /* No L2 TAP on this OS — the overlay hides the toggle, the config
     * field stays a no-op, and Net4CPC stays on the host-socket
     * fallback unconditionally. Silently return. */
    (void)cfg; (void)cli_tap_dev;
    return;
#else
    const bool want_auto =
        cfg->net4cpc && cfg->net4cpc_tap &&
        (!cli_tap_dev || !cli_tap_dev[0]);
    const bool want_cli =
        cli_tap_dev && cli_tap_dev[0] && cfg->net4cpc;

    /* Idempotent: if the currently-active backend already matches what
     * cfg asks for, do nothing. Cold-boot saves on unrelated fields
     * re-enter this path; tearing down and re-creating the tap every
     * time would re-prompt for pkexec on every overlay save. */
    static bool have_auto = false;
    static char have_cli_name[64] = "";
    if (want_auto && have_auto) return;
    if (want_cli && strncmp(have_cli_name, cli_tap_dev, sizeof(have_cli_name)) == 0)
        return;
    if (!want_auto && !want_cli && !have_auto && !have_cli_name[0])
        return;

    /* Detach from any previously-attached tap. We deliberately do NOT
     * delete the kernel device — leaving it lets the next enable
     * (this run or a future launch) attach without re-prompting. */
    if (have_auto) {
        net4cpc_attach_tap(NULL);
        n4c_stack_set_dhcp_enabled(false);
        n4c_stack_set_dns_enabled(false);
        have_auto = false;
    }
    if (have_cli_name[0]) {
        net4cpc_attach_tap(NULL);
        have_cli_name[0] = '\0';
    }

    if (want_cli) {
        if (net4cpc_attach_tap(cli_tap_dev) < 0) {
            fprintf(stderr, "1984: TAP attach failed, "
                    "Net4CPC will use the legacy host-socket fallback.\n");
            return;
        }
        snprintf(have_cli_name, sizeof(have_cli_name), "%s", cli_tap_dev);
        return;
    }

    if (!want_auto) return;

    /* Linux's ip(8) accepts any device name; we use 'cpc-tap0' so the
     * device is visibly attributable. BSD if_tap drivers (FreeBSD,
     * NetBSD, OpenBSD) only accept names of the form 'tap<digits>'
     * because the name is used to select the clone driver; pick a
     * portable 'tap0' there. */
#if defined(__linux__)
    static const char auto_name[] = "cpc-tap0";
#else
    static const char auto_name[] = "tap0";
#endif
    /* Build "host_ip/cidr" from host_ip and netmask dotted-quads.
     * Count contiguous set bits to get the prefix length. */
    unsigned a, b, c, d;
    int cidr = 24;
    if (sscanf(cfg->net4cpc_tap_netmask, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        u32 mask = (a << 24) | (b << 16) | (c << 8) | d;
        cidr = 0; while (mask & 0x80000000u) { cidr++; mask <<= 1; }
    }
    char host_cidr[40];
    snprintf(host_cidr, sizeof(host_cidr), "%s/%d",
             cfg->net4cpc_tap_host_ip, cidr);
    if (tap_auto_create(auto_name, host_cidr) < 0) {
        fprintf(stderr, "1984: auto TAP setup failed; Net4CPC stays on "
                "the legacy host-socket fallback.\n");
        return;
    }
    if (net4cpc_attach_tap(auto_name) < 0) {
        fprintf(stderr, "1984: TAP attach failed after auto-create.\n");
        return;
    }
    n4c_stack_set_dhcp_params(cfg->net4cpc_tap_host_ip,
                              cfg->net4cpc_tap_netmask,
                              cfg->net4cpc_tap_lease_start,
                              cfg->net4cpc_tap_lease_end);
    n4c_stack_set_dhcp_enabled(true);
    n4c_stack_set_dns_enabled(true);
    have_auto = true;
#endif /* TAP_SUPPORTED */
}

static void apply_led_enables(const Config *cfg) {
    /* Floppies: shown when an FDC is wired (664/6128 built-in; 464 needs DDI-1). */
    bool fdc_present = (cfg->model == MODEL_6128) || (cfg->model == MODEL_664) || cfg->dd1;
    leds_set_enabled(LED_FDC_A, fdc_present);
    leds_set_enabled(LED_FDC_B, fdc_present);
    /* MX4 expansions need both rom_board and the expansion toggle. */
    bool mx4 = cfg->rom_board;
    leds_set_enabled(LED_IDE, mx4 && cfg->symbiface_ide);
    leds_set_enabled(LED_USB, mx4 && cfg->albireo);
    leds_set_enabled(LED_SD,  false);            /* retired — replaced by LED_M4 */
    leds_set_enabled(LED_M4,  mx4 && cfg->m4);
    leds_set_enabled(LED_NET, mx4 && cfg->net4cpc);
    leds_set_enabled(LED_USIFAC, mx4 && cfg->usifac);
    /* Printer is gated on the MX4 expansion bus (parallel port wired
     * through the MX4 connector in this emulator). */
    leds_set_enabled(LED_PRINTER, mx4);
}

/* OS title is just "1984" now; the model name and F-key hints render
 * inside the SDL window in the top-left header drawn each frame. */
#define TITLE_NORMAL_464  "1984"
#define TITLE_NORMAL_664  "1984"
#define TITLE_NORMAL_6128 "1984"
#define TITLE_CAPTURED    "Mouse captured  |  Ctrl+Enter to release"

static const char *title_for_model(CpcModel m) {
    switch (m) {
        case MODEL_464:  return TITLE_NORMAL_464;
        case MODEL_664:  return TITLE_NORMAL_664;
        default:         return TITLE_NORMAL_6128;
    }
}

static void set_mouse_capture(SDL_Window *win, bool capture, bool *flag, CpcModel model) {
    SDL_SetWindowRelativeMouseMode(win, capture);
    *flag = capture;
    SDL_SetWindowTitle(win, capture ? TITLE_CAPTURED : title_for_model(model));
}

static void usage(const char *prog, int code) {
    FILE *out = code ? stderr : stdout;
    fprintf(out,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --464               Boot as CPC 464 (overrides config)\n"
        "  --664               Boot as CPC 664 (overrides config)\n"
        "  --6128              Boot as CPC 6128 (overrides config)\n"
        "  --dd1               Enable DDI-1 floppy interface on CPC 464 (overrides config)\n"
        "  --memory=KB         RAM size: 64, 128, 256, 512 or 576 (overrides config)\n"
        "  --config=PATH       Use PATH as the config file instead of ~/.config/1984/1984.conf\n"
        "                      (read-only; config_save still targets the default path)\n"
        "  --disk-a=PATH       Mount a DSK image in drive A (overrides config)\n"
        "  --disk-b=PATH       Mount a DSK image in drive B (overrides config)\n"
        "  --rom-os=PATH       Replace the OS (lower) ROM image\n"
        "  --rom-slot=N:PATH   Load a ROM image into upper ROM slot N (0-31)\n"
        "                      May be specified multiple times\n"
        "  --trace-io          Log CRTC/Gate Array register writes to stderr\n"
        "  --trace-palette     Log palette writes and the firmware-flush fallback to stderr\n"
        "  --trace-input       Log keyboard and joystick events to stderr (row 9 scans, gamepad/joystick events, key up/down)\n"
        "  --trace-m4          Log every M4 board command and response to stderr\n"
        "  --trace-symbos-msg  Log SymbOS RST #10 message sends (net-daemon range) to stderr\n"
        "  --trace-albireo     Log every Albireo (CH376) command and response to stderr\n"
        "  --trace-net4cpc     Log every Net4CPC (W5100S) register access and socket command to stderr\n"
        "  --trace-tap         Log TAP-backed network stack events (ARP, IP, UDP, ICMP, TCP) to stderr\n"
        "  --autostart=NAME    After boot, types run\"NAME into BASIC\n"
        "  --paste=TEXT        After boot, types TEXT verbatim (\\n becomes Enter)\n"
        "  --load-sna=PATH     Load an Amstrad .sna snapshot file after init (.sna v1-v3)\n"
        "  --tap=DEVNAME       Bind Net4CPC to a Linux TAP device for real LAN access.\n"
        "                      Pass --tap= (empty) to let the kernel auto-name (tap0…).\n"
        "                      Needs CAP_NET_ADMIN or a pre-created persistent TAP.\n"
        "  --save-sna-at=N:PATH  Save a .sna snapshot at frame N (typically pairs with --paste / --autostart)\n"
        "  --save-sna-at-ide=N:PATH  Save a .sna snapshot when the Nth ATA command is issued (for cross-emulator bisection)\n"
        "  --screenshot-at=N:PATH  Save a screenshot at frame N to PATH, then exit\n"
        "  --joy-script=SPEC   Scripted joystick (row 9). SPEC = comma-separated DIRS:FRAMES\n"
        "                      steps; DIRS = u d l r 1 2 (Fire1/2) or - (neutral).\n"
        "                      e.g. --joy-script=d:150,-:30,u:150 (down, rest, up)\n"
        "  --gif-out=PATH      Record a GIF from boot (finalised on exit; pairs with --exit-after)\n"
        "  --exit-after=N      Quit after frame N (deterministic headless capture)\n"
        "  --printer-pdf=DIR   Capture parallel-printer output to timestamped PDFs in DIR\n"
        "  --printer-real      Spool captured pages to the host's default CUPS printer (lp)\n"
        "  --symbols=PATH      Load an SDCC .map file and annotate the disassembler/monitor\n"
        "                      output with the closest preceding symbol. Repeatable.\n"
        "                      Use --symbols=HEX:PATH to apply the map only when the live\n"
        "                      RAM bank (GA MMR) equals HEX, useful for paged OSes like FUZIX.\n"
        "  --monitor-pty       Open a PTY for the memory monitor (minicom -b 9600 -D <path>)\n"
        "  --kbd-pty           Open a PTY that injects writes as keystrokes and streams\n"
        "                      the firmware text-out (&BB5A) for external test harnesses\n"
        "  --ocr-monitor       Adds an in-memory screen-text reader: each frame we scan\n"
        "                      video RAM, decode against the firmware font, and stream the\n"
        "                      80x25 (or 40x25) char grid out the kbd PTY on change. Lets\n"
        "                      probes follow CP/M+ output that bypasses &BB5A. Implies\n"
        "                      --kbd-pty.\n"
        "  -h, --help          Show this help and exit\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  F4     Save screenshot (.ppm)\n"
        "  F5     Warm reset\n"
        "  F6     Toggle video capture (.gif in CWD)\n"
        "  F8     Open/close memory monitor / disassembler\n"
        "  F9     Options overlay\n"
        "  F11    Toggle fullscreen\n"
        "  F12    Quit\n"
        "  Ctrl+V Paste clipboard text into the emulator\n"
        "\n"
        "Configuration file: ~/.config/1984/1984.conf\n",
        prog);
    exit(code);
}

int main(int argc, char *argv[]) {

    SD_INIT();
    SD_LOG("main() entered, argc=%d", argc);
    net_compat_init();   /* initialise Winsock (no-op on POSIX) */
    SD_LOG("net_compat_init done");

    const char *config_path     = NULL;   /* --config=PATH overrides ~/.config/1984/1984.conf */
    const char *autostart       = NULL;
    const char *paste_arg       = NULL;
    const char *load_sna_arg    = NULL;
    const char *tap_dev_arg     = NULL;   /* --tap=DEVNAME for Net4CPC backend */
    const char *disk_a_arg      = NULL;
    const char *disk_b_arg      = NULL;
    const char *rom_os_arg      = NULL;
    int         screenshot_frame = -1;
    const char *screenshot_path  = NULL;
    int         save_sna_frame   = -1;
    const char *save_sna_path    = NULL;
    int         save_sna_ide_cmd = -1;
    const char *save_sna_ide_path = NULL;
    const char *joyscript_arg    = NULL;  /* --joy-script=SPEC: scripted joystick */
    const char *gifout_arg       = NULL;  /* --gif-out=PATH: record a GIF from boot */
    int         exit_after       = -1;    /* --exit-after=N: quit at frame N */
    const char *printer_pdf_dir_arg = NULL; /* --printer-pdf=DIR */
    bool        printer_real_arg = false;   /* --printer-real */
    bool        trace_io         = false;
    bool        monitor_pty      = false;
    bool        kbd_pty_enabled  = false;
    bool        ocr_monitor_enabled = false;
    CpcModel    model_override   = (CpcModel)-1;  /* -1 = no override */
    bool        dd1_override     = false;
    int         memory_override  = 0;             /* 0 = no override */

    /* --rom-slot=N:PATH pairs collected from CLI */
    struct { int slot; const char *path; } rom_slots[ROM_EXT_COUNT];
    int rom_slot_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            usage(argv[0], 0);
        else if (strncmp(argv[i], "--config=", 9) == 0 && argv[i][9] != '\0')
            config_path = argv[i] + 9;
        else if (strncmp(argv[i], "--autostart=", 12) == 0 && argv[i][12] != '\0')
            autostart = argv[i] + 12;
        else if (strncmp(argv[i], "--paste=", 8) == 0 && argv[i][8] != '\0')
            paste_arg = argv[i] + 8;
        else if (strncmp(argv[i], "--load-sna=", 11) == 0 && argv[i][11] != '\0')
            load_sna_arg = argv[i] + 11;
        else if (strncmp(argv[i], "--tap=", 6) == 0)
            tap_dev_arg = argv[i] + 6;   /* may be empty: kernel auto-names */
        else if (strncmp(argv[i], "--disk-a=", 9) == 0 && argv[i][9] != '\0')
            disk_a_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--disk-b=", 9) == 0 && argv[i][9] != '\0')
            disk_b_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--rom-os=", 9) == 0 && argv[i][9] != '\0')
            rom_os_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--rom-slot=", 11) == 0 && argv[i][11] != '\0') {
            const char *arg = argv[i] + 11;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --rom-slot requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            int slot = atoi(arg);
            if (slot < 0 || slot >= ROM_EXT_COUNT) {
                fprintf(stderr, "%s: --rom-slot slot number must be 0-%d\n",
                        argv[0], ROM_EXT_COUNT - 1);
                usage(argv[0], 1);
            }
            if (rom_slot_count < ROM_EXT_COUNT) {
                rom_slots[rom_slot_count].slot = slot;
                rom_slots[rom_slot_count].path = colon + 1;
                rom_slot_count++;
            }
        } else if (strcmp(argv[i], "--464") == 0) {
            model_override = MODEL_464;
        } else if (strcmp(argv[i], "--664") == 0) {
            model_override = MODEL_664;
        } else if (strcmp(argv[i], "--6128") == 0) {
            model_override = MODEL_6128;
        } else if (strcmp(argv[i], "--dd1") == 0) {
            dd1_override = true;
        } else if (strncmp(argv[i], "--memory=", 9) == 0 && argv[i][9] != '\0') {
            int kb = atoi(argv[i] + 9);
            if (kb != 64 && kb != 128 && kb != 256 && kb != 512 && kb != 576) {
                fprintf(stderr, "%s: --memory=KB must be 64, 128, 256, 512 or 576\n", argv[0]);
                return 2;
            }
            memory_override = kb;
        } else if (strncmp(argv[i], "--symbols=", 10) == 0 && argv[i][10] != '\0') {
            /* Two forms:
             *   --symbols=PATH               (matches any MMR)
             *   --symbols=HEX:PATH           (only when ram_bank == HEX)
             * Repeatable. */
            const char *arg = argv[i] + 10;
            const char *colon = strchr(arg, ':');
            int mmr = SYMBOLS_ANY_MMR;
            const char *path = arg;
            if (colon) {
                /* Try parsing the prefix as a hex byte. If it fails (e.g.
                 * a Windows path like C:\), fall back to "any MMR". */
                char *endp = NULL;
                long v = strtol(arg, &endp, 16);
                if (endp == colon && v >= 0 && v <= 0xFF) {
                    mmr  = (int)v;
                    path = colon + 1;
                }
            }
            symbols_load(path, mmr);
        } else if (strcmp(argv[i], "--monitor-pty") == 0) {
            monitor_pty = true;
        } else if (strcmp(argv[i], "--kbd-pty") == 0) {
            kbd_pty_enabled = true;
        } else if (strcmp(argv[i], "--ocr-monitor") == 0) {
            kbd_pty_enabled = true;   /* OCR output piggybacks on the kbd PTY */
            ocr_monitor_enabled = true;
        } else if (strcmp(argv[i], "--trace-io") == 0) {
            trace_io = true;
        } else if (strcmp(argv[i], "--trace-palette") == 0) {
            cpc_trace_palette = 1;
        } else if (strcmp(argv[i], "--trace-input") == 0) {
            cpc_trace_input = 1;
        } else if (strcmp(argv[i], "--trace-m4") == 0) {
            m4_trace = 1;
        } else if (strcmp(argv[i], "--trace-symbos-msg") == 0) {
            symbos_trace_enable();
        } else if (strcmp(argv[i], "--trace-albireo") == 0) {
            ch376_trace = 1;
        } else if (strcmp(argv[i], "--trace-net4cpc") == 0) {
            net4cpc_trace = 1;
        } else if (strcmp(argv[i], "--trace-tap") == 0) {
            n4c_stack_trace = 1;
        } else if (strncmp(argv[i], "--screenshot-at=", 16) == 0 && argv[i][16] != '\0') {
            const char *arg = argv[i] + 16;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --screenshot-at requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            screenshot_frame = atoi(arg);
            screenshot_path  = colon + 1;
        } else if (strncmp(argv[i], "--joy-script=", 13) == 0 && argv[i][13] != '\0') {
            joyscript_arg = argv[i] + 13;
        } else if (strncmp(argv[i], "--gif-out=", 10) == 0 && argv[i][10] != '\0') {
            gifout_arg = argv[i] + 10;
        } else if (strncmp(argv[i], "--printer-pdf=", 14) == 0 && argv[i][14] != '\0') {
            printer_pdf_dir_arg = argv[i] + 14;
        } else if (strcmp(argv[i], "--printer-real") == 0) {
            printer_real_arg = true;
        } else if (strncmp(argv[i], "--exit-after=", 13) == 0 && argv[i][13] != '\0') {
            exit_after = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "--save-sna-at=", 14) == 0 && argv[i][14] != '\0') {
            const char *arg = argv[i] + 14;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --save-sna-at requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            save_sna_frame = atoi(arg);
            save_sna_path  = colon + 1;
        } else if (strncmp(argv[i], "--save-sna-at-ide=", 18) == 0 && argv[i][18] != '\0') {
            const char *arg = argv[i] + 18;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --save-sna-at-ide requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            save_sna_ide_cmd = atoi(arg);
            save_sna_ide_path = colon + 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unrecognised option '%s'\n", argv[0], argv[i]);
            usage(argv[0], 1);
        }
    }

    SD_LOG("args parsed, calling config_load");
    Config cfg;
    if (config_load_from(&cfg, config_path) < 0) {
        SD_LOG("config_load FAILED (returned <0)");
        return 1;
    }
    SD_LOG("config_load OK: model=%d mem=%d", (int)cfg.model, cfg.memory_kb);
    /* Master debug enable from config. When off (default), dbg_getenv()
     * returns NULL for every call, neutering all ONE_K_TRACE_* /
     * ONE_K_CAP_TEXT / ONE_K_DUMP_* / ONE_K_RESET_SNA / ONE_K_TRACE_PANIC
     * hooks. Production env vars (CC_TABLES, FAKE_RTC, AUTOSTART_FRAMES,
     * PASTE_GAP) use plain getenv() and are unaffected. */
    g_debug_enabled = cfg.debug ? 1 : 0;

    /* Apply per-board ROM templates to rom_ext[]: every slot tagged
     * with an active board (m4/albireo/symbiface_ide-for-cyboard) gets
     * the board's template path loaded; slots whose only board is now
     * inactive get cleared. See config_apply_boards() and #103. */
    config_apply_boards(&cfg);
    SD_LOG("  rom_os=%s", cfg.rom_os);
    SD_LOG("  rom_basic=%s", cfg.rom_basic);
    SD_LOG("  rom_amsdos=%s [next: SDL_Init]", cfg.rom_amsdos);

    if (model_override != (CpcModel)-1)
        config_set_model(&cfg, model_override);
    if (memory_override)
        cfg.memory_kb = memory_override;
    if (dd1_override)
        config_apply_dd1(&cfg, true);
    if (disk_a_arg) snprintf(cfg.disk_a, sizeof(cfg.disk_a), "%s", disk_a_arg);
    if (disk_b_arg) snprintf(cfg.disk_b, sizeof(cfg.disk_b), "%s", disk_b_arg);
    if (rom_os_arg) snprintf(cfg.rom_os, sizeof(cfg.rom_os), "%s", rom_os_arg);
    if (printer_pdf_dir_arg) {
        snprintf(cfg.pdf_printer_dir, sizeof(cfg.pdf_printer_dir), "%s", printer_pdf_dir_arg);
        cfg.pdf_printer = true;
    }
    if (printer_real_arg) {
        cfg.print_sink  = PRINTER_SINK_REAL_PRINTER;
        cfg.pdf_printer = true;   /* needed so capture is active */
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SD_LOG("calling SDL_Init(VIDEO|AUDIO|GAMEPAD)");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SD_LOG("SDL_Init FAILED: %s", SDL_GetError());
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SD_LOG("SDL_Init OK; calling cpc_init");

    /* static: the CPC struct embeds 1 MB of RAM plus ROMs and buffers,
     * which exceeds the default 1 MB thread stack on Windows. BSS keeps
     * the same scope without stack pressure or heap bookkeeping. */
    static CPC cpc;
    if (cpc_init(&cpc, cfg.model, cfg.rom_os, cfg.rom_basic, cfg.scale) < 0) {
        SD_LOG("cpc_init FAILED (rom_os=%s rom_basic=%s)", cfg.rom_os, cfg.rom_basic);
        fprintf(stderr, "Failed to initialise CPC (check ROM paths in ~/.config/1984/1984.conf)\n");
        SDL_Quit();
        return 1;
    }
    SD_LOG("cpc_init OK");
    ga_set_monochrome(&cpc.ga, cfg.monochrome);
    cpc.mem.ram_size    = cfg.memory_kb * 1024;
    cpc.mx4             = cfg.mx4;
    cpc.net4cpc         = cfg.net4cpc;
    cpc.rtc             = cfg.rtc;

    net4cpc_tap_sync(&cfg, tap_dev_arg);
    /* These four expansions install their drivers as upper ROMs, so without
     * the Roms Board fitted they can't run — force them off in the live CPC
     * state while leaving the cfg values intact (re-enabling Roms Board
     * restores them from cfg on the next cold boot). */
    cpc.symbiface_ide   = cfg.symbiface_ide   && cfg.rom_board;
    cpc.symbiface_mouse = cfg.symbiface_mouse && cfg.rom_board;
    cpc.m4              = cfg.m4              && cfg.rom_board;
    cpc.symbnet         = cfg.symbnet;
    if (cfg.m4 && cfg.m4_path[0])
        snprintf(cpc.m4_card.root, M4_PATH_MAX, "%s", cfg.m4_path);
    if (cfg.m4)
        m4_set_image(&cpc.m4_card, cfg.m4_image);
    if (cfg.symbiface_ide && cfg.ide_image[0])
        ide_open(&cpc.ide_chip, cfg.ide_image);
    cpc.albireo       = cfg.albireo && cfg.rom_board;
    cpc.albireo_mouse = cpc.albireo && cfg.albireo_mouse;
    cpc.ch376.has_mouse   = cpc.albireo_mouse;
    cpc.ch376_b.has_mouse = false;
    if (cpc.albireo && cfg.albireo_image[0]) {
        CH376 *storage = cpc.albireo_mouse ? &cpc.ch376_b : &cpc.ch376;
        ch376_open(storage, cfg.albireo_image);
    }
    ch376_disable_disk_read = cfg.albireo_disable_disk_read ? 1 : 0;
    /* M4 and Albireo share the 0xFExx port range — Albireo wins if both set. */
    if (cpc.albireo && cpc.m4) {
        cpc.m4 = false;
        cfg.m4 = false;
    }
    usifac_init(&cpc.usifac, cfg.usifac, cfg.usifac_backend, cfg.usifac_tcp_port,
                cfg.usifac_pty_link);
    perryfi_init(&cpc.perryfi, cfg.mx4 && cfg.usifac && cfg.perryfi);
    usifac_attach_perryfi(&cpc.usifac, &cpc.perryfi);
    printer_set_connected(&cpc.printer, cfg.mx4);
    printer_set_pdf_output_dir(&cpc.printer, cfg.pdf_printer_dir);
    printer_set_pdf_enabled(&cpc.printer, cfg.pdf_printer && cfg.pdf_printer_dir[0]);
    printer_set_sink(&cpc.printer,
                     cfg.print_sink == PRINTER_SINK_REAL_PRINTER ? PRINT_SINK_REAL
                                                                 : PRINT_SINK_PDF);
    apply_led_enables(&cfg);
    /* Cassette: always wired on 464; requires external_tape toggle on 664/6128. */
    if (cfg.tape[0] &&
            (cpc.model == MODEL_464 || cfg.external_tape))
        tape_load(&cpc.tape, cfg.tape);

    /* Load AMSDOS ROM (non-fatal — 464 doesn't need it) */
    if (cfg.rom_amsdos[0])
        mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);

    /* Load expansion ROMs into slots 0-31 (from config) when the ROM Board
     * is fitted. With rom_board=false the cfg paths are preserved but
     * unused — re-enabling restores the prior layout from a single source
     * of truth. */
    if (cfg.rom_board) {
        for (int s = 0; s < ROM_EXT_COUNT; s++) {
            /* Slot 7 is loaded below when M4 is enabled — skip any stale config entry */
            if (s == M4_ROM_SLOT && cfg.m4) continue;
            if (cfg.rom_ext[s][0])
                mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
        }
    }
    /* Load M4ROM into its dedicated slot when M4 is enabled */
    if (cfg.m4) {
        char m4rom[512];
        config_default_m4rom(m4rom, sizeof(m4rom));
        mem_load_rom_ext(&cpc.mem, M4_ROM_SLOT, m4rom);
        /* Seed the M4 board's config buffer (read via bus bypass at 0xF400)
         * with the ROM's default contents so reads return valid pointers
         * (e.g. runfile_ptr → autoexec_fn) before any C_CONFIG write. */
        memcpy(cpc.m4_card.cfg_mem,
               &cpc.mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
               sizeof(cpc.m4_card.cfg_mem));
        /* Patch M4ROM's helper-pointer table NOW so SymbOS netd-m4c.exe's
         * m4crom routine picks up our trap addresses on its first read.
         * Without this the daemon copies the original M4ROM helper entries
         * and bypasses our trap, breaking the bulk transfer needed by
         * GETNETWORK and every TCP send/recv. */
        m4_install_helper_shim(&cpc.m4_card, &cpc.mem);
    }

    /* Apply --rom-slot=N:PATH overrides from CLI */
    for (int i = 0; i < rom_slot_count; i++) {
        if (mem_load_rom_ext(&cpc.mem, rom_slots[i].slot, rom_slots[i].path) < 0)
            fprintf(stderr, "1984: failed to load ROM slot %d: %s\n",
                    rom_slots[i].slot, rom_slots[i].path);
    }

    /* Load floppy images from config */
    if (cfg.disk_a[0]) {
        if (disk_load(&cpc.drive[0], cfg.disk_a) < 0) {
            fprintf(stderr, "1984: failed to load drive A: %s\n", cfg.disk_a);
            cfg.disk_a[0] = '\0';
        }
    }
    if (cfg.disk_b[0]) {
        if (disk_load(&cpc.drive[1], cfg.disk_b) < 0) {
            fprintf(stderr, "1984: failed to load drive B: %s\n", cfg.disk_b);
            cfg.disk_b[0] = '\0';
        }
    }

    /* Camera shutter SFX — loaded from embedded WAV data in shutter_wav.h.
     * sfx_buf/sfx_buf_len hold the decoded PCM so it can be replayed on each F4 press.
     * A dedicated logical device stream is opened; SDL3 mixes it with the PSG stream. */
    SDL_AudioStream *sfx_stream = NULL;
    Uint8  *sfx_buf     = NULL;
    Uint32  sfx_buf_len = 0;
    {
        SDL_AudioSpec sfx_spec;
        SDL_IOStream *io = SDL_IOFromConstMem(shutter_wav, shutter_wav_len);
        if (io && SDL_LoadWAV_IO(io, true, &sfx_spec, &sfx_buf, &sfx_buf_len)) {
            sfx_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sfx_spec, NULL, NULL);
            if (sfx_stream)
                SDL_ResumeAudioStreamDevice(sfx_stream);
        }
    }

    SD_LOG("ROMs/disks loaded; calling overlay_init");
    Overlay overlay;
    overlay_init(&overlay, &cfg, &cpc);
    SD_LOG("overlay_init OK");

    HostMount host_mount = {0};   /* F10 toggle state; see host_mount.c */

    Monitor *monitor = monitor_create(&cpc);
    if (monitor_pty) {
        const char *pty_path = monitor_pty_open(monitor);
        if (pty_path)
            fprintf(stderr, "1984: monitor PTY: %s  (minicom -b 9600 -D %s)\n",
                    pty_path, pty_path);
        else
            fprintf(stderr, "1984: failed to open monitor PTY\n");
    }

    Paste paste;
    paste_init(&paste);

    if (kbd_pty_enabled) {
        const char *p = kbd_pty_open();
        if (p) fprintf(stderr, "1984: kbd PTY: %s\n", p);
        else   fprintf(stderr, "1984: failed to open kbd PTY\n");
    }
    if (ocr_monitor_enabled) {
        screen_text_init(&cpc);
        screen_text_set_enabled(true);
        fprintf(stderr, "1984: OCR monitor enabled (screen-text → kbd PTY)\n");
    }

    Joy joy;
    joy_init(&joy);

    /* --joy-script: deterministic scripted joystick (row 9), the joystick analogue
     * of --paste. Drives automated UI tests headlessly (e.g. sweep the GEOBENCH
     * pointer to a screen edge to reproduce a moving-cursor artifact). */
    JoyScript joyscript;
    bool have_joyscript = false;
    if (joyscript_arg) {
        if (joyscript_init(&joyscript, joyscript_arg)) {
            have_joyscript = true;
            fprintf(stderr, "[joy-script] %d step(s) queued\n", joyscript.nsteps);
        } else {
            fprintf(stderr, "%s: invalid --joy-script spec\n", argv[0]);
            return 1;
        }
    }

    /* Load a snapshot AFTER all machine init (ROMs, IDE, Albireo, joysticks
     * etc.) is done — the snapshot overrides only the CPU + GA + CRTC + PPI
     * + RAM state. Suppress autostart/paste so a typed sequence doesn't fight
     * the loaded snapshot's PC. */
    if (load_sna_arg) {
        if (snapshot_load(&cpc, load_sna_arg) == 0) {
            autostart = NULL;
            paste_arg = NULL;
        }
    }

    bool fullscreen     = cfg.fullscreen;
    bool mouse_captured = false;
    display_set_smoothing(&cpc.display, cfg.fullscreen_smoothing);
    if (fullscreen)
        SDL_SetWindowFullscreen(cpc.display.window, true);

    /* Frames to wait before injecting autostart text (matches caprice32 timing).
     * ONE_K_AUTOSTART_FRAMES overrides for headless QA scripts that need a
     * larger margin (paste before BASIC Ready is silently dropped). */
    int autostart_countdown = (autostart || paste_arg) ? 42 : 0;
    {
        const char *e = getenv("ONE_K_AUTOSTART_FRAMES");
        if (e && autostart_countdown > 0) {
            int n = atoi(e);
            if (n > 0) autostart_countdown = n;
        }
    }

    /* 50 Hz frame pacer — audio is pushed every 20 ms, matching the CPC's PAL rate.
     * VSync is off; we sleep for any leftover time in each 20 ms budget. */
#define FRAME_NS 20000000ULL
    Uint64 next_frame = SDL_GetTicksNS();

    int  frame_count = 0;
    bool running = true;
    if (gifout_arg)            /* --gif-out: record from boot (finalised on exit) */
        videocap_start(gifout_arg);
    SD_LOG("entering main loop — startup complete");
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* #129 instrumentation: log every host-side SDL event with
             * frame number + a short type name. Used to test the
             * hypothesis that interactive boots are non-deterministic
             * because spurious window/keyboard/mouse events from X11
             * fire at variable frames and shift HDCPM's boot path.
             * Gated on ONE_K_TRACE_SDL_EV so it costs nothing in
             * normal use. */
            if (dbg_getenv("ONE_K_TRACE_SDL_EV")) {
                const char *name = NULL;
                switch (ev.type) {
                case SDL_EVENT_QUIT:                  name = "QUIT"; break;
                case SDL_EVENT_KEY_DOWN:              name = "KEY_DOWN"; break;
                case SDL_EVENT_KEY_UP:                name = "KEY_UP"; break;
                case SDL_EVENT_MOUSE_MOTION:          name = "MOUSE_MOTION"; break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:     name = "MOUSE_BUTTON_DOWN"; break;
                case SDL_EVENT_MOUSE_BUTTON_UP:       name = "MOUSE_BUTTON_UP"; break;
                case SDL_EVENT_MOUSE_WHEEL:           name = "MOUSE_WHEEL"; break;
                case SDL_EVENT_WINDOW_SHOWN:          name = "WINDOW_SHOWN"; break;
                case SDL_EVENT_WINDOW_HIDDEN:         name = "WINDOW_HIDDEN"; break;
                case SDL_EVENT_WINDOW_EXPOSED:        name = "WINDOW_EXPOSED"; break;
                case SDL_EVENT_WINDOW_MOVED:          name = "WINDOW_MOVED"; break;
                case SDL_EVENT_WINDOW_RESIZED:        name = "WINDOW_RESIZED"; break;
                case SDL_EVENT_WINDOW_MINIMIZED:      name = "WINDOW_MINIMIZED"; break;
                case SDL_EVENT_WINDOW_MAXIMIZED:      name = "WINDOW_MAXIMIZED"; break;
                case SDL_EVENT_WINDOW_RESTORED:       name = "WINDOW_RESTORED"; break;
                case SDL_EVENT_WINDOW_MOUSE_ENTER:    name = "WINDOW_MOUSE_ENTER"; break;
                case SDL_EVENT_WINDOW_MOUSE_LEAVE:    name = "WINDOW_MOUSE_LEAVE"; break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:   name = "WINDOW_FOCUS_GAINED"; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:     name = "WINDOW_FOCUS_LOST"; break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:name = "WINDOW_CLOSE_REQUESTED"; break;
                case SDL_EVENT_DROP_FILE:             name = "DROP_FILE"; break;
                case SDL_EVENT_GAMEPAD_ADDED:         name = "GAMEPAD_ADDED"; break;
                case SDL_EVENT_GAMEPAD_REMOVED:       name = "GAMEPAD_REMOVED"; break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:   name = "GAMEPAD_BUTTON_DOWN"; break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:     name = "GAMEPAD_BUTTON_UP"; break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:   name = "GAMEPAD_AXIS_MOTION"; break;
                case SDL_EVENT_GAMEPAD_REMAPPED:      name = "GAMEPAD_REMAPPED"; break;
                case SDL_EVENT_KEYBOARD_ADDED:        name = "KEYBOARD_ADDED"; break;
                case SDL_EVENT_KEYBOARD_REMOVED:      name = "KEYBOARD_REMOVED"; break;
                case SDL_EVENT_MOUSE_ADDED:           name = "MOUSE_ADDED"; break;
                case SDL_EVENT_MOUSE_REMOVED:         name = "MOUSE_REMOVED"; break;
                case SDL_EVENT_JOYSTICK_ADDED:        name = "JOYSTICK_ADDED"; break;
                case SDL_EVENT_JOYSTICK_REMOVED:      name = "JOYSTICK_REMOVED"; break;
                case SDL_EVENT_TEXT_INPUT:            name = "TEXT_INPUT"; break;
                case SDL_EVENT_TEXT_EDITING:          name = "TEXT_EDITING"; break;
                }
                if (name) {
                    /* For KEY events log the scancode + repeat flag so
                     * we can spot auto-repeat decay from before launch. */
                    if (ev.type == SDL_EVENT_KEY_DOWN ||
                        ev.type == SDL_EVENT_KEY_UP) {
                        fprintf(stderr, "[SDL_EV] frame=%d type=%s scancode=%u repeat=%u\n",
                                frame_count, name,
                                (unsigned)ev.key.scancode,
                                (unsigned)ev.key.repeat);
                    } else {
                        fprintf(stderr, "[SDL_EV] frame=%d type=%s\n",
                                frame_count, name);
                    }
                } else {
                    fprintf(stderr, "[SDL_EV] frame=%d type=0x%X (other)\n",
                            frame_count, (unsigned)ev.type);
                }
                fflush(stderr);
            }

            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }

            /* Mouse events when captured — route to CPC mouse state, skip other handlers */
            if (mouse_captured) {
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if (cpc.symbiface_mouse)
                        mouse_move(&cpc.mouse, (int)ev.motion.xrel, (int)ev.motion.yrel);
                    if (cpc.albireo_mouse)
                        ch376_mouse_move(&cpc.ch376, (int)ev.motion.xrel, (int)ev.motion.yrel);
                    continue;
                }
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                              (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                              (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : -1;
                    bool pressed = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    if (btn >= 0) {
                        if (cpc.symbiface_mouse)
                            mouse_button(&cpc.mouse, btn, pressed);
                        if (cpc.albireo_mouse)
                            ch376_mouse_button(&cpc.ch376, btn, pressed);
                    }
                    continue;
                }
                if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (cpc.symbiface_mouse)
                        mouse_scroll(&cpc.mouse, (int)ev.wheel.y);
                    continue;
                }
                /* Ctrl+Enter releases mouse capture — do not pass Enter to CPC */
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_RETURN &&
                    (SDL_GetModState() & SDL_KMOD_CTRL)) {
                    set_mouse_capture(cpc.display.window, false,
                                      &mouse_captured, cpc.model);
                    continue;
                }
            }

            /* Click in emulator window captures mouse when either the
             * SYMBiFACE II PS/2 mouse or the Albireo USB HID mouse is
             * enabled. Ctrl+Enter releases capture. */
            if (!mouse_captured && (cpc.symbiface_mouse || cpc.albireo_mouse) &&
                !overlay.visible &&
                ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                ev.button.windowID == SDL_GetWindowID(cpc.display.window)) {
                set_mouse_capture(cpc.display.window, true,
                                  &mouse_captured, cpc.model);
                int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                          (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                          (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : -1;
                if (btn >= 0) {
                    if (cpc.symbiface_mouse)
                        mouse_button(&cpc.mouse, btn, true);
                    if (cpc.albireo_mouse)
                        ch376_mouse_button(&cpc.ch376, btn, true);
                }
                continue;
            }

            /* Joystick/gamepad events */
            if (joy_handle_event(&joy, &ev, &cpc.kbd))
                continue;
            /* Monitor window gets its own events */
            if (monitor_handle_event(monitor, &ev))
                continue;
            /* F9 opens overlay — release mouse capture first so UI is usable */
            if (mouse_captured && ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_F9)
                set_mouse_capture(cpc.display.window, false,
                                  &mouse_captured, cpc.model);
            /* Overlay gets first crack at every key event */
            if (overlay_handle_event(&overlay, &ev))
                continue;
            /* Pass remaining key events to the CPC */
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                /* Ctrl + or Ctrl - : step the window size by one integer
                 * scale unit (1× CPC native … 4×). The SDL key for "=/+"
                 * is EQUALS regardless of shift state; KP_PLUS / KP_MINUS
                 * cover the numeric keypad. */
                bool ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
                bool key_plus  = (ev.key.scancode == SDL_SCANCODE_EQUALS
                               || ev.key.scancode == SDL_SCANCODE_KP_PLUS);
                bool key_minus = (ev.key.scancode == SDL_SCANCODE_MINUS
                               || ev.key.scancode == SDL_SCANCODE_KP_MINUS);
                if (ctrl && (key_plus || key_minus)) {
                    cfg.scale += key_plus ? 1 : -1;
                    if (cfg.scale < 1) cfg.scale = 1;
                    if (cfg.scale > 4) cfg.scale = 4;
                    SDL_SetWindowSize(cpc.display.window,
                                      WINDOW_W * cfg.scale,
                                      WINDOW_H * cfg.scale + LED_BAR_HEIGHT);
                    continue;
                }
                if (ev.key.scancode == SDL_SCANCODE_F12) {
                    running = false;
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    if (monitor_is_open(monitor))
                        monitor_handle_event(monitor,
                            &(SDL_Event){.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                                         .window.windowID=monitor_window_id(monitor)});
                    else
                        monitor_open(monitor);
                } else if (ev.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(cpc.display.window, fullscreen);
                } else if (ev.key.scancode == SDL_SCANCODE_F4) {
                    char path[256];
                    char tmp[256];
                    strncpy(tmp, argv[0], sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    snprintf(path, sizeof(path), "%s_%ld.ppm", basename(tmp), (long)time(NULL));
                    display_save_ppm(&cpc.display, path);
                    if (sfx_stream && sfx_buf) {
                        SDL_ClearAudioStream(sfx_stream);
                        SDL_PutAudioStreamData(sfx_stream, sfx_buf, (int)sfx_buf_len);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F5) {
                    cpc_reset(&cpc);
                } else if (ev.key.scancode == SDL_SCANCODE_F10) {
                    /* Toggle host-side card browse: pause the guest, mount
                     * every active FAT card image (M4 SD / IDE / Albireo)
                     * under /run/user/$UID/1984/<label>/ and open the host
                     * file manager. Press F10 again to unmount + cold-boot
                     * so the guest's stale FAT cache is dropped on resume. */
                    if (host_mount.active) {
                        host_mount_close(&host_mount);
                        cpc.paused = false;
                        overlay.needs_cold_boot = true;
                    } else if (host_mount_supported()) {
                        cpc.paused = true;
                        if (host_mount_open(&host_mount, &cfg)) {
                            host_mount.active = true;
                        } else {
                            cpc.paused = false;
                            fprintf(stderr,
                                "F10: nothing to mount (no active FAT card image)\n");
                        }
                    } else {
                        fprintf(stderr,
                            "F10: needs libguestfs (guestmount, guestunmount) and xdg-open\n");
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F6) {
                    /* Toggle video capture. Auto-name in CWD when starting
                     * outside the overlay file picker. */
                    if (videocap_active()) {
                        videocap_stop();
                    } else {
                        char path[256];
                        time_t t = time(NULL);
                        struct tm *lt = localtime(&t);
                        if (lt)
                            strftime(path, sizeof(path),
                                     "1984-%Y%m%d-%H%M%S.gif", lt);
                        else
                            snprintf(path, sizeof(path), "1984-capture.gif");
                        videocap_start(path);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_V &&
                           (SDL_GetModState() & SDL_KMOD_CTRL)) {
                    /* Release Ctrl from the CPC matrix before injecting text;
                     * otherwise the first character arrives as Ctrl+key. */
                    cpc_key_event(&cpc, SDL_SCANCODE_LCTRL, false);
                    cpc_key_event(&cpc, SDL_SCANCODE_RCTRL, false);
                    char *text = SDL_GetClipboardText();
                    if (text) { paste_text_raw(&paste, text); SDL_free(text); }
                } else {
                    if (cpc_trace_input)
                        fprintf(stderr, "[input] KEY_DOWN scancode=%d name=%s\n",
                                ev.key.scancode, SDL_GetScancodeName(ev.key.scancode));
                    cpc_key_event(&cpc, ev.key.scancode, true);
                }
            } else if (ev.type == SDL_EVENT_KEY_UP) {
                if (cpc_trace_input)
                    fprintf(stderr, "[input] KEY_UP   scancode=%d\n", ev.key.scancode);
                cpc_key_event(&cpc, ev.key.scancode, false);
            }
        }

        if (autostart_countdown > 0 && --autostart_countdown == 0) {
            if (paste_arg) {
                /* --paste=TEXT: expand \n to real newline then inject */
                char buf[512];
                int j = 0;
                for (const char *p = paste_arg; *p && j < (int)sizeof(buf)-2; p++) {
                    if (p[0] == '\\' && p[1] == 'n') { buf[j++] = '\r'; buf[j++] = '\n'; p++; }
                    else buf[j++] = *p;
                }
                buf[j] = '\0';
                paste_text(&paste, buf);
            } else {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "run\"%s", autostart);
                paste_text(&paste, cmd);
            }
        }

        overlay_tick(&overlay);

        /* Auto-finish F10 mount session if the user ejected the card from
         * the file manager (Nautilus, Files, …). Same effect as a second
         * F10 press: tear down anything that's still mounted, unpause,
         * cold-boot to drop the guest's stale FAT cache. */
        if (host_mount.active &&
                host_mount_externally_unmounted(&host_mount)) {
            host_mount_close(&host_mount);
            cpc.paused = false;
            overlay.needs_cold_boot = true;
        }

        if (overlay.needs_cold_boot) {
            overlay.needs_cold_boot = false;
            cpc.model = cfg.model;
            cpc.mem.ram_size = cfg.memory_kb * 1024;
            mem_load_rom(&cpc.mem, cfg.rom_os, cfg.rom_basic);
            /* AMSDOS is built-in on 6128 and supplied by DDI-1 on the 464;
             * config_set_model / config_apply_dd1 already nail rom_amsdos
             * to the right value, so just mirror it into the live ROM map. */
            if (cfg.rom_amsdos[0])
                mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);
            else
                mem_unload_amsdos(&cpc.mem);
            /* First unload every slot — covers the rom_board=true→false
             * transition. The cfg.rom_ext paths stay intact so the next
             * re-enable reloads them. */
            for (int s = 0; s < ROM_EXT_COUNT; s++)
                mem_unload_rom_ext(&cpc.mem, s);
            if (cfg.rom_board) {
                for (int s = 0; s < ROM_EXT_COUNT; s++) {
                    if (s == M4_ROM_SLOT && cfg.m4) continue;
                    if (cfg.rom_ext[s][0])
                        mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
                }
            }
            if (cfg.m4) {
                char m4rom[512];
                config_default_m4rom(m4rom, sizeof(m4rom));
                mem_load_rom_ext(&cpc.mem, M4_ROM_SLOT, m4rom);
                memcpy(cpc.m4_card.cfg_mem,
                       &cpc.mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
                       sizeof(cpc.m4_card.cfg_mem));
                m4_install_helper_shim(&cpc.m4_card, &cpc.mem);
            }
            SDL_SetWindowTitle(cpc.display.window, title_for_model(cpc.model));
            cpc.mx4              = cfg.mx4;
            cpc.net4cpc          = cfg.net4cpc;
            cpc.rtc              = cfg.rtc;
            net4cpc_tap_sync(&cfg, tap_dev_arg);
            /* See boot-time comment: these four need the Roms Board. */
            cpc.symbiface_ide    = cfg.symbiface_ide   && cfg.rom_board;
            cpc.symbiface_mouse  = cfg.symbiface_mouse && cfg.rom_board;
            cpc.m4               = cfg.m4              && cfg.rom_board;
            cpc.symbnet          = cfg.symbnet;
            if (cpc.m4 && cfg.m4_path[0])
                snprintf(cpc.m4_card.root, M4_PATH_MAX, "%s", cfg.m4_path);
            if (cpc.m4)
                m4_set_image(&cpc.m4_card, cfg.m4_image);
            ide_close(&cpc.ide_chip);
            if (cpc.symbiface_ide && cfg.ide_image[0])
                ide_open(&cpc.ide_chip, cfg.ide_image);
            cpc.albireo       = cfg.albireo && cfg.rom_board;
            cpc.albireo_mouse = cpc.albireo && cfg.albireo_mouse;
            cpc.ch376.has_mouse   = cpc.albireo_mouse;
            cpc.ch376_b.has_mouse = false;
            ch376_close(&cpc.ch376);
            ch376_close(&cpc.ch376_b);
            if (cpc.albireo && cfg.albireo_image[0]) {
                CH376 *storage = cpc.albireo_mouse ? &cpc.ch376_b : &cpc.ch376;
                ch376_open(storage, cfg.albireo_image);
            }
            ch376_disable_disk_read = cfg.albireo_disable_disk_read ? 1 : 0;
            usifac_shutdown(&cpc.usifac);
            usifac_init(&cpc.usifac, cfg.usifac,
                        cfg.usifac_backend, cfg.usifac_tcp_port,
                        cfg.usifac_pty_link);
            perryfi_shutdown(&cpc.perryfi);
            perryfi_init(&cpc.perryfi,
                         cfg.mx4 && cfg.usifac && cfg.perryfi);
            usifac_attach_perryfi(&cpc.usifac, &cpc.perryfi);
            if (cpc.albireo && cpc.m4) {
                cpc.m4 = false;
                cfg.m4 = false;
            }
            apply_led_enables(&cfg);
            tape_eject(&cpc.tape);
            if (cfg.tape[0] &&
                    (cpc.model == MODEL_464 || cfg.external_tape))
                tape_load(&cpc.tape, cfg.tape);
            /* Release mouse capture on cold boot */
            if (mouse_captured)
                set_mouse_capture(cpc.display.window, false,
                                  &mouse_captured, cpc.model);
            net4cpc_reset();
            cpc_reset(&cpc);
        }

        monitor_pty_tick(monitor);
        kbd_pty_tick(&paste);
        screen_text_tick(&cpc);
        paste_tick(&paste, &cpc.kbd);
        if (have_joyscript) joyscript_tick(&joyscript, &cpc.kbd);
        bool was_paused   = cpc.paused;
        bool was_stepping = cpc.step_once;
        net4cpc_poll();
        usifac_poll(&cpc.usifac);
        perryfi_poll(&cpc.perryfi);
        printer_tick(&cpc.printer);
        cpc_frame(&cpc);
        /* Auto-open monitor on breakpoint hit */
        if (!was_paused && cpc.paused) {
            /* Debug hook: ONE_K_DUMP_RAM=/path/to/file writes physical RAM
             * to a file at every breakpoint pause (overwrites). Lets us
             * diff our RAM against WinAPE .sna byte-for-byte. */
            const char *dump_path = dbg_getenv("ONE_K_DUMP_RAM");
            if (dump_path) {
                FILE *fp = fopen(dump_path, "wb");
                if (fp) {
                    fwrite(cpc.mem.ram, 1, cpc.mem.ram_size, fp);
                    fclose(fp);
                    fprintf(stderr, "[ONE_K_DUMP_RAM] wrote %u bytes to %s at PC=%04X\n",
                            cpc.mem.ram_size, dump_path, cpc.cpu.pc);
                }
            }
            monitor_open(monitor);
            monitor_notify_break(monitor);
        } else if (was_stepping && cpc.paused) {
            monitor_notify_step(monitor);
        }
        /* DUMP_VIDEO_RAM=/path/file — once per frame, writes the base 64 KB
         * of RAM (the only memory the CRTC ever sees) plus the CRTC reg state
         * appended after byte 0x10000. Last frame wins; quit the emulator with
         * the corrupted screen still on display and the file captures it. */
        {
            const char *vram_path = dbg_getenv("DUMP_VIDEO_RAM");
            if (vram_path) {
                FILE *fp = fopen(vram_path, "wb");
                if (fp) {
                    fwrite(cpc.mem.ram, 1, cpc.mem.ram_size, fp);
                    fwrite(cpc.crtc.reg, 1, sizeof(cpc.crtc.reg), fp);
                    fclose(fp);
                }
            }
        }
        /* Video capture: grab the CPC framebuffer before the overlay
         * draws on top. GIF decimates to 25 fps; WebM takes every
         * frame at 50 fps (VP9 compresses interframe deltas well). */
        if (g_videocap_gif) {
            if ((g_videocap_skip ^= 1) == 0)
                gifcap_frame(g_videocap_gif, cpc.display.pixels);
        }
        if (g_videocap_webm) {
            if (!webmcap_frame(g_videocap_webm, cpc.display.pixels))
                videocap_stop();
        }
        /* Paused-state visual: greyscale the frozen frame and re-composite
         * it each iteration (cpc_frame early-returns when paused, so it
         * stops calling display_upload itself — the back buffer would
         * otherwise show undefined content after RenderPresent). The
         * PAUSED label is drawn below after overlay_render so it ends
         * up on top of everything. */
        static bool prev_paused = false;
        if (cpc.paused) {
            if (!prev_paused) display_apply_greyscale(&cpc.display);
            display_upload(&cpc.display);
        }
        prev_paused = cpc.paused;
        overlay_render(&overlay, cpc.display.renderer);
        if (cpc.paused)
            display_draw_paused_label(&cpc.display);
        /* Debug-mode FPS overlay (bottom-left, just above the LED bar).
         * Doubles as a visual marker that debug machinery is live. */
        if (g_debug_enabled) {
            static Uint64 last_ns = 0;
            static int    samples = 0;
            static float  fps_smooth = 0.0f;
            Uint64 now = SDL_GetTicksNS();
            if (last_ns) {
                float dt_s = (now - last_ns) / 1.0e9f;
                if (dt_s > 0.0f) {
                    float fps_inst = 1.0f / dt_s;
                    /* exponential smoothing — alpha=0.05 ≈ 1 s window at 50 Hz */
                    if (samples == 0) fps_smooth = fps_inst;
                    else              fps_smooth = fps_smooth * 0.95f + fps_inst * 0.05f;
                    samples++;
                }
            }
            last_ns = now;
            int ww, wh;
            SDL_GetWindowSize(cpc.display.window, &ww, &wh);
            int bar_h = 24; /* LED_BAR_HEIGHT; keep in sync with display.c */
            if (bar_h > wh / 4) bar_h = wh / 4;
            char buf[64];
            snprintf(buf, sizeof(buf), "DBG  %.1f fps", (double)fps_smooth);
            /* black drop-shadow for readability over any background */
            SDL_SetRenderDrawColor(cpc.display.renderer, 0, 0, 0, 255);
            SDL_RenderDebugText(cpc.display.renderer, 7.0f,
                                (float)(wh - bar_h - 12), buf);
            SDL_SetRenderDrawColor(cpc.display.renderer, 0xFF, 0xC0, 0x40, 255);
            SDL_RenderDebugText(cpc.display.renderer, 6.0f,
                                (float)(wh - bar_h - 13), buf);
            (void)ww;
        }
        /* In-window footer strip: a dark band just above the LED bar
         * with the model name in red+bold and F-key hints in white.
         * The OS title bar stays minimal ("1984"). */
        {
            const char *model_str = "CPC 6128";
            if (cpc.model == MODEL_464) model_str = "CPC 464";
            else if (cpc.model == MODEL_664) model_str = "CPC 664";
            const char *keys =
                "  F4=screenshot  F5=reset  F6=capture  F8=monitor  "
                "F9=options  F10=open image  F11=fullscreen  F12=quit";

            int hdr_ww, hdr_wh;
            SDL_GetWindowSize(cpc.display.window, &hdr_ww, &hdr_wh);
            const float strip_h = 16.0f;
            const float led_bar = 24.0f; /* LED_BAR_HEIGHT */
            float strip_y = (float)hdr_wh - led_bar - strip_h;
            float text_y  = strip_y + 4.0f;
            SDL_FRect strip = { 0.0f, strip_y, (float)hdr_ww, strip_h };
            SDL_SetRenderDrawBlendMode(cpc.display.renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(cpc.display.renderer, 0x10, 0x10, 0x14, 255);
            SDL_RenderFillRect(cpc.display.renderer, &strip);

            /* Centre the model+keys block horizontally. SDL3's debug
             * font cell is 8 px wide. */
            float text_w = (float)(strlen(model_str) + strlen(keys)) * 8.0f;
            float model_x = ((float)hdr_ww - text_w) * 0.5f;
            if (model_x < 0.0f) model_x = 0.0f;
            float keys_x  = model_x + (float)strlen(model_str) * 8.0f;

            /* Model name in bold-red: render twice with 1-px X offset so
             * the SDL debug font looks heavier. */
            SDL_SetRenderDrawColor(cpc.display.renderer, 0xFF, 0x40, 0x40, 255);
            SDL_RenderDebugText(cpc.display.renderer, model_x,        text_y, model_str);
            SDL_RenderDebugText(cpc.display.renderer, model_x + 1.0f, text_y, model_str);

            /* F-key hints: light grey, drawn right after the model string. */
            SDL_SetRenderDrawColor(cpc.display.renderer, 0xE0, 0xE0, 0xE0, 255);
            SDL_RenderDebugText(cpc.display.renderer, keys_x, text_y, keys);
        }
        display_flip(&cpc.display);
        monitor_render(monitor);

        frame_count++;
        if (exit_after >= 0 && frame_count >= exit_after)
            running = false;        /* --exit-after: deterministic headless quit */
        if (trace_io && frame_count == 210)
            cpc_trace_io = 1;
        if (trace_io && frame_count == 600)
            cpc_trace_io = 0;
        /* At frame 700, dump what the GA palette looks like and sample screen */
        if (trace_io && frame_count == 700) {
            fprintf(stderr, "=== GA palette (inks) ===\n");
            for (int i = 0; i <= 16; i++)
                fprintf(stderr, "  ink[%2d] = hw%d\n", i, cpc.ga.ink[i]);
            /* Histogram of screen byte values */
            int hist[256] = {0};
            for (int i = 0; i < 0x4000; i++) hist[cpc.mem.ram[0xC000+i]]++;
            fprintf(stderr, "=== Screen byte histogram (non-zero) ===\n");
            for (int i = 1; i < 256; i++)
                if (hist[i]) fprintf(stderr, "  [%02X] = %d\n", i, hist[i]);
        }
        if (screenshot_frame >= 0 && frame_count == screenshot_frame) {
            display_save_ppm(&cpc.display, screenshot_path);
            if (dbg_getenv("ONE_K_DUMP_PC")) {
                fprintf(stderr, "[pc-dump] frame=%d PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IFF1=%d halted=%d\n",
                        frame_count, cpc.cpu.pc, cpc.cpu.sp, cpc.cpu.af,
                        cpc.cpu.bc, cpc.cpu.de, cpc.cpu.hl,
                        (int)cpc.cpu.iff1, (int)cpc.cpu.halted);
                fprintf(stderr, "[pc-dump] stack: ");
                for (int i = 0; i < 16; i++) {
                    u16 a = (u16)(cpc.cpu.sp + i);
                    fprintf(stderr, "%02X ", cpc.mem.ram[a & 0xFFFF]);
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "[pc-dump] code at PC (Z80 view): ");
                for (int i = 0; i < 16; i++) {
                    u16 a = (u16)(cpc.cpu.pc + i);
                    fprintf(stderr, "%02X ", cpc.bus.mem_read(cpc.bus.ctx, a));
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "[pc-dump] code at 0x100: ");
                for (int i = 0; i < 64; i++) {
                    fprintf(stderr, "%02X ", cpc.bus.mem_read(cpc.bus.ctx, 0x100 + i));
                }
                fprintf(stderr, "\n");
                fprintf(stderr, "[pc-dump] mem BE00-BE7F (Z80 view):\n");
                for (int row = 0; row < 8; row++) {
                    fprintf(stderr, "[pc-dump] %04X: ", 0xBE00 + row*16);
                    for (int col = 0; col < 16; col++) {
                        fprintf(stderr, "%02X ", cpc.bus.mem_read(cpc.bus.ctx, 0xBE00 + row*16 + col));
                    }
                    fprintf(stderr, "\n");
                }
            }
            running = false;
        }
        if (save_sna_frame >= 0 && frame_count == save_sna_frame) {
            snapshot_save(&cpc, save_sna_path);
            /* Don't exit — let the user combine with --screenshot-at if they
             * want both. If they want to exit after the snapshot they can use
             * a screenshot-at at the same frame, or rely on --exit-after. */
            save_sna_frame = -1;
        }
        if (save_sna_ide_cmd >= 0) {
            extern u32 ide_cmd_count_for_crash_trace;
            if ((int)ide_cmd_count_for_crash_trace >= save_sna_ide_cmd) {
                snapshot_save(&cpc, save_sna_ide_path);
                save_sna_ide_cmd = -1;
            }
        }

        /* Sleep for whatever is left of the 20 ms frame budget */
        next_frame += FRAME_NS;
        Uint64 now = SDL_GetTicksNS();
        if (now < next_frame) {
            SDL_DelayNS(next_frame - now);
        } else if (now > next_frame + 3 * FRAME_NS) {
            next_frame = now; /* reset if more than 3 frames behind */
        }
    }

    if (mouse_captured)
        SDL_SetWindowRelativeMouseMode(cpc.display.window, false);
    paste_free(&paste);
    joy_destroy(&joy);
    monitor_destroy(monitor);
    if (sfx_stream) SDL_DestroyAudioStream(sfx_stream);
    if (sfx_buf)    SDL_free(sfx_buf);
    videocap_stop();   /* finalise GIF if recording */
    printer_shutdown(&cpc.printer);
    perryfi_shutdown(&cpc.perryfi);
    cpc_destroy(&cpc);
    SDL_Quit();
    return 0;
}
