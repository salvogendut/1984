#include "config.h"
#include "m4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>      /* strcasecmp */
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include "compat_win.h"   /* mkdir(mode) shim on Windows */

void config_set_model(Config *cfg, CpcModel model);  /* defined below */

/* Per-user config base directory. Wine never sets $HOME inside the guest
 * process — only %APPDATA% / %USERPROFILE% / HOMEDRIVE+HOMEPATH are
 * exposed — so on Windows we look those up explicitly. Returns NULL only
 * if no usable env var is set (effectively never on a sane account).
 *
 * Layout written by config_save() / read by config_load():
 *   Linux/BSD/macOS/Haiku: $HOME/.config/1984/1984.conf
 *   Windows:               %APPDATA%/1984/1984.conf     (no .config)
 *
 * Issue #106: this used to be a single getenv("HOME") which silently
 * returned NULL on Windows, making every config save a no-op. */
static const char *config_home_env(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
#ifdef _WIN32
    h = getenv("USERPROFILE");
    if (h && *h) return h;
    /* HOMEDRIVE+HOMEPATH fallback would need composition; APPDATA-based
     * paths are built directly in config_dir() so this NULL is fine. */
#endif
    return NULL;
}

/* Write the per-user 1984 config directory (no trailing slash, no
 * filename) into out[sz]. Returns 0 on success, -1 if no usable env
 * var is set. When `make_dirs` is true, creates intermediate
 * directories so a subsequent fopen("…/1984.conf", "w") will succeed
 * on a fresh account. */
static int config_dir(char *out, size_t sz, bool make_dirs) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        snprintf(out, sz, "%s/1984", appdata);
        if (make_dirs) mkdir(out, 0755);
        return 0;
    }
    const char *up = config_home_env();
    if (!up) return -1;
    if (make_dirs) {
        char tmp[CONFIG_PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/.config", up); mkdir(tmp, 0755);
    }
    snprintf(out, sz, "%s/.config/1984", up);
    if (make_dirs) mkdir(out, 0755);
    return 0;
#else
    const char *home = config_home_env();
    if (!home) return -1;
    if (make_dirs) {
        char tmp[CONFIG_PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/.config", home); mkdir(tmp, 0755);
    }
    snprintf(out, sz, "%s/.config/1984", home);
    if (make_dirs) mkdir(out, 0755);
    return 0;
#endif
}

#define ROM_FILE_OS_464      "OS_464.ROM"
#define ROM_FILE_BASIC_464   "BASIC_1.0.ROM"
#define ROM_FILE_OS_664      "OS_664.ROM"
#define ROM_FILE_BASIC_664   "BASIC_664.ROM"   /* 664 BASIC v1.1.0 — NOT the same as BASIC_1.1.ROM (which is v1.2.0) */
#define ROM_FILE_OS_6128     "OS_6128.ROM"
#define ROM_FILE_BASIC_6128  "BASIC_1.1.ROM"   /* misnamed file — actually v1.2.0 */
#define ROM_FILE_AMSDOS      "AMSDOS.ROM"
#define ROM_FILE_AMSDOS_664  "AMSDOS_664.ROM"
#define ROM_FILE_M4ROM      "M4ROM.ROM"
#define ROM_FILE_DIAG       "AmstradDiagLower.rom"

/* Build a path to a bundled ROM file.
 * Priority:
 *   1. ~/.config/1984/roms/<file>      — user override
 *   2. $(pkgdatadir)/roms/<file>       — system install (from autotools)
 *   3. ./roms/<file>                   — dev tree / cwd fallback
 * The first path that exists is returned; otherwise the user-config path
 * (so a "file not found" error names the location the user is expected
 * to populate). */
static void rom_cfg_path(const char *file, char *out, size_t size) {
    char user[512], system_[512];
    char cdir[CONFIG_PATH_MAX];

    user[0] = '\0';
    if (config_dir(cdir, sizeof(cdir), false) == 0)
        snprintf(user, sizeof(user), "%s/roms/%s", cdir, file);

    if (user[0] && access(user, R_OK) == 0) {
        snprintf(out, size, "%s", user);
        return;
    }
#ifdef ROM_INSTALL_DIR
    snprintf(system_, sizeof(system_), "%s/%s", ROM_INSTALL_DIR, file);
    if (access(system_, R_OK) == 0) {
        snprintf(out, size, "%s", system_);
        return;
    }
#else
    (void)system_;
#endif
    if (access("roms", R_OK) == 0) {
        char dev_[512];
        snprintf(dev_, sizeof(dev_), "roms/%s", file);
        if (access(dev_, R_OK) == 0) {
            snprintf(out, size, "%s", dev_);
            return;
        }
    }
    /* Nothing found — return the user-config path so any error message
     * points the user at the location they should populate. */
    snprintf(out, size, "%s",
             user[0] ? user : (const char *)file);
}

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->scale     = 1;
    cfg->mx4       = true;   /* expansion bus connected by default */
    cfg->rom_board = true;   /* ROM Board fitted by default */
    cfg->fullscreen_smoothing = true;  /* preserve historic linear-scale behaviour */
    cfg->real_crt = false;
    cfg->crt_scanlines = DISPLAY_CRT_SCANLINES_DEFAULT;
    cfg->crt_brightness = DISPLAY_CRT_BRIGHTNESS_DEFAULT;
    cfg->crt_contrast = DISPLAY_CRT_CONTRAST_DEFAULT;
    cfg->tinker    = false;
    cfg->debug     = false;
    snprintf(cfg->net4cpc_tap_host_ip,    sizeof(cfg->net4cpc_tap_host_ip),    "10.0.0.1");
    snprintf(cfg->net4cpc_tap_netmask,    sizeof(cfg->net4cpc_tap_netmask),    "255.255.255.0");
    snprintf(cfg->net4cpc_tap_lease_start, sizeof(cfg->net4cpc_tap_lease_start), "10.0.0.100");
    snprintf(cfg->net4cpc_tap_lease_end,  sizeof(cfg->net4cpc_tap_lease_end),  "10.0.0.150");
    snprintf(cfg->usifac_backend, sizeof(cfg->usifac_backend), "pty");
    cfg->usifac_tcp_port = 4001;
    cfg->usifac_pty_link[0] = '\0';
    cfg->perryfi = false;
    cfg->audio_volume    = 80;
    cfg->audio_stereo_sep = 0;
    config_set_model(cfg, MODEL_6128);  /* sets model, memory, OS, BASIC, AMSDOS */
}

/* Expand a leading ~ to the home directory. Result written into out[size]. */
static void expand_path(const char *in, char *out, size_t size) {
    if (in[0] == '~') {
        const char *home = config_home_env();
        if (home)
            snprintf(out, size, "%s%s", home, in + 1);
        else
            snprintf(out, size, "%s", in);
    } else {
        snprintf(out, size, "%s", in);
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static const char *mono_to_str(MonoMode m) {
    switch (m) {
        case MONO_GREEN: return "green";
        case MONO_AMBER: return "amber";
        case MONO_WHITE: return "white";
        default:         return "off";
    }
}

static bool parse_mono(const char *val, MonoMode *out) {
    if (!strcasecmp(val, "off"))   { *out = MONO_OFF;   return true; }
    if (!strcasecmp(val, "green")) { *out = MONO_GREEN; return true; }
    if (!strcasecmp(val, "amber")) { *out = MONO_AMBER; return true; }
    if (!strcasecmp(val, "white")) { *out = MONO_WHITE; return true; }
    return false;
}

static bool parse_bool(const char *val, bool *out) {
    if (!strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes")) {
        *out = true; return true;
    }
    if (!strcmp(val, "false") || !strcmp(val, "0") || !strcmp(val, "no")) {
        *out = false; return true;
    }
    return false;
}

static void config_create_default(const char *path) {
    char dir[CONFIG_PATH_MAX];
    if (config_dir(dir, sizeof(dir), true) != 0) return;

    /* Resolve the actual best path for each default ROM so the generated
     * config matches what the binary would load. Without this, the file
     * was hard-coded to paths under ~/.config/1984/roms/ — fine if the
     * user has put ROMs there, broken on a fresh RPM install where the
     * ROMs live under the system data dir. */
    char os_path[CONFIG_PATH_MAX], basic_path[CONFIG_PATH_MAX], amsdos_path[CONFIG_PATH_MAX];
    rom_cfg_path(ROM_FILE_OS_6128,    os_path,     sizeof(os_path));
    rom_cfg_path(ROM_FILE_BASIC_6128, basic_path,  sizeof(basic_path));
    rom_cfg_path(ROM_FILE_AMSDOS,     amsdos_path, sizeof(amsdos_path));

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "1984: could not create config file %s\n", path);
        return;
    }

    fprintf(f,
        "# 1984 CPC Emulator — configuration file\n"
        "# Edit and restart the emulator for changes to take effect.\n"
        "\n"
        "[machine]\n"
        "# CPC model: 464, 664 or 6128\n"
        "model=6128\n"
        "# RAM size in KB: 64 (CPC 464/664) or 128+ (CPC 6128)\n"
        "memory=128\n"
        "\n"
        "[roms]\n"
        "# Paths to ROM images. ~ is expanded to your home directory.\n"
        "os=%s\n"
        "basic=%s\n"
        "amsdos=%s\n"
        "\n",
        os_path, basic_path, amsdos_path);

    fprintf(f, "%s",
        "[expansion_roms]\n"
        "# Load extra ROMs into upper ROM slots 0-31.\n"
        "# slot_7 is AMSDOS by default; leave entries empty to use defaults.\n"
        "# Example: slot_5=/path/to/TOOLKIT.ROM\n"
        "\n"
        "[storage]\n"
        "# Paths to .dsk floppy images (leave empty for no disk)\n"
        "drive_a=\n"
        "drive_b=\n"
        "# .cdt tape image. Always wired on CPC 464; on CPC 6128 needs\n"
        "# external_tape=true (below) so the deck is virtually plugged in.\n"
        "tape=\n"
        "external_tape=false\n"
        "\n"
        "[hardware]\n"
        "# mx4: MX4 expansion bus — when false, all extension peripherals\n"
        "# (M4, Net4CPC, RTC, SYMBiFACE, Albireo, …) are disconnected.\n"
        "mx4=true\n"
        "# rom_board: emulate the expansion ROM board (32 upper-ROM slots).\n"
        "# When false, only OS + BASIC + AMSDOS are loaded; the slot_N entries\n"
        "# below are remembered but ignored until re-enabled.\n"
        "rom_board=true\n"
        "# dd1: CPC 464 only — DDI-1 floppy interface (enables drives + AMSDOS)\n"
        "dd1=false\n"
        "# Optional expansion hardware (not yet implemented)\n"
        "m4=false\n"
        "m4_path=\n"
        "m4_image=\n"
        "# USIfAC II serial interface (port &FBD0/D1)\n"
        "usifac=false\n"
        "usifac_backend=pty\n"
        "usifac_tcp_port=4001\n"
        "# Optional stable symlink to the USIfAC PTY's /dev/pts/N slave\n"
        "# (e.g. /tmp/usifac.pty). Empty = no alias.\n"
        "usifac_pty_link=\n"
        "# PerryFi Wi-Fi AT-modem (gated on usifac=true).\n"
        "perryfi=false\n"
        "net4cpc=false\n"
        "net4cpc_tap=false\n"
        "net4cpc_tap_host_ip=10.0.0.1\n"
        "net4cpc_tap_netmask=255.255.255.0\n"
        "net4cpc_tap_lease_start=10.0.0.100\n"
        "net4cpc_tap_lease_end=10.0.0.150\n"
        "rtc=false\n"
        "symbiface_ide=false\n"
        "ide_image=\n"
        "symbiface_mouse=false\n"
        "symbnet=false\n"
        "albireo=false\n"
        "albireo_image=\n"
        "albireo_mouse=false\n"
        "albireo_disable_disk_read=false\n"
        "\n"
        "[display]\n"
        "# Window scale factor: 1, 2, or 3\n"
        "scale=2\n"
        "# Start in fullscreen mode: true or false\n"
        "fullscreen=false\n"
        "# Smooth (linear) vs sharp (nearest) texture scaling\n"
        "fullscreen_smoothing=true\n"
        "# Lightweight CRT post-process. real_crt enables the overlay controls.\n"
        "real_crt=false\n"
        "# Scanline opacity, 0..95. Brightness is 50..100. Contrast is 50..150.\n"
        "crt_scanlines=35\n"
        "crt_brightness=100\n"
        "crt_contrast=100\n"
        "# Monochrome monitor tint: off | green | amber | white\n"
        "# Maps the CPC palette to shades of one phosphor colour.\n"
        "monochrome=off\n"
        "\n"
        "[audio]\n"
        "# Output volume 0..100 (perceptual curve)\n"
        "audio_volume=80\n"
        "# Stereo separation 0..255 — 0=mono, 255=full Caprice32 ABC panning\n"
        "# (channel A left, B centre, C right)\n"
        "audio_stereo_sep=0\n"
        "\n"
        "[advanced]\n"
        "# Enable the Advanced overlay tab with low-level toggles\n"
        "tinker=false\n"
        "# Enable debug machinery (ONE_K_TRACE_* env vars, panic dumps,\n"
        "# text capture, etc.). Off by default; when off, none of the\n"
        "# debug machinery runs and ONE_K_* trace env vars are no-ops.\n"
        "debug=false\n"
    );

    fclose(f);
    fprintf(stderr, "1984: created default config at %s\n", path);
}

int config_load(Config *cfg) {
    return config_load_from(cfg, NULL);
}

int config_load_from(Config *cfg, const char *path_override) {
    config_defaults(cfg);

    char path[CONFIG_PATH_MAX];
    bool using_override = (path_override && *path_override);

    if (using_override) {
        snprintf(path, sizeof(path), "%s", path_override);
    } else {
        char cdir[CONFIG_PATH_MAX];
        if (config_dir(cdir, sizeof(cdir), false) != 0) return 0;
        snprintf(path, sizeof(path), "%s/1984.conf", cdir);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        if (using_override) {
            fprintf(stderr, "1984: --config: cannot open '%s'\n", path);
            return -1;
        }
        config_create_default(path);
        return 0;
    }

    char line[512];
    char section[64] = "";
    int  lineno = 0;
    int  rc = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (!strcmp(section, "machine")) {
            if (!strcmp(key, "model")) {
                if (!strcmp(val, "464"))       cfg->model = MODEL_464;
                else if (!strcmp(val, "664"))  cfg->model = MODEL_664;
                else if (!strcmp(val, "6128")) cfg->model = MODEL_6128;
                else { fprintf(stderr, "1984.conf:%d: unknown model '%s', using default\n", lineno, val); }
            } else if (!strcmp(key, "memory")) {
                int kb = atoi(val);
                if (kb == 64 || kb == 128 || kb == 256 || kb == 512 || kb == 576
                        || kb == 768 || kb == 1024)
                    cfg->memory_kb = kb;
                else { fprintf(stderr, "1984.conf:%d: invalid memory '%s', using default (%d KB)\n", lineno, val, cfg->memory_kb); }
            }
        } else if (!strcmp(section, "roms")) {
            if (!strcmp(key, "os"))
                expand_path(val, cfg->rom_os, sizeof(cfg->rom_os));
            else if (!strcmp(key, "basic"))
                expand_path(val, cfg->rom_basic, sizeof(cfg->rom_basic));
            else if (!strcmp(key, "amsdos"))
                expand_path(val, cfg->rom_amsdos, sizeof(cfg->rom_amsdos));
        } else if (!strcmp(section, "expansion_roms")) {
            if (strncmp(key, "slot_", 5) == 0) {
                int slot = atoi(key + 5);
                if (slot >= 0 && slot < ROM_EXT_COUNT && val[0])
                    expand_path(val, cfg->rom_ext[slot], sizeof(cfg->rom_ext[slot]));
            }
        } else if (strncmp(section, "board:", 6) == 0) {
            const char *board = section + 6;
            char (*tbl)[CONFIG_PATH_MAX] = config_board_slots(cfg, board);
            if (tbl && strncmp(key, "slot_", 5) == 0) {
                int slot = atoi(key + 5);
                if (slot >= 0 && slot < ROM_EXT_COUNT && val[0]) {
                    expand_path(val, tbl[slot], sizeof(tbl[slot]));
                    /* Add board name to the slot's board CSV, dedup'd. */
                    if (!config_boards_contains(cfg->rom_ext_boards[slot], board)) {
                        size_t cur = strlen(cfg->rom_ext_boards[slot]);
                        size_t need = cur + (cur ? 1 : 0) + strlen(board) + 1;
                        if (need <= sizeof(cfg->rom_ext_boards[slot])) {
                            if (cur) cfg->rom_ext_boards[slot][cur++] = ',';
                            strcpy(cfg->rom_ext_boards[slot] + cur, board);
                        }
                    }
                }
            } else if (!strcmp(key, "image")) {
                char *img = config_board_image(cfg, board);
                if (img && val[0])
                    expand_path(val, img, CONFIG_PATH_MAX);
            }
        } else if (!strcmp(section, "storage")) {
            if (!strcmp(key, "drive_a"))
                expand_path(val, cfg->disk_a, sizeof(cfg->disk_a));
            else if (!strcmp(key, "drive_b"))
                expand_path(val, cfg->disk_b, sizeof(cfg->disk_b));
            else if (!strcmp(key, "tape"))
                expand_path(val, cfg->tape, sizeof(cfg->tape));
            else if (!strcmp(key, "external_tape")) {
                bool b;
                if (parse_bool(val, &b)) cfg->external_tape = b;
                else { fprintf(stderr, "1984.conf:%d: external_tape must be true/false\n", lineno); rc = -1; }
            }
        } else if (!strcmp(section, "hardware")) {
            bool b;
            if (!strcmp(key, "mx4")) {
                if (parse_bool(val, &b)) cfg->mx4 = b;
                else { fprintf(stderr, "1984.conf:%d: mx4 must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "rom_board")) {
                if (parse_bool(val, &b)) cfg->rom_board = b;
                else { fprintf(stderr, "1984.conf:%d: rom_board must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "dd1")) {
                if (parse_bool(val, &b)) cfg->dd1 = b;
                else { fprintf(stderr, "1984.conf:%d: dd1 must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "m4")) {
                if (parse_bool(val, &b)) cfg->m4 = b;
                else { fprintf(stderr, "1984.conf:%d: m4 must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "m4_path")) {
                snprintf(cfg->m4_path, CONFIG_PATH_MAX, "%s", val);
            } else if (!strcmp(key, "m4_image")) {
                snprintf(cfg->m4_image, CONFIG_PATH_MAX, "%s", val);
            } else if (!strcmp(key, "usifac")) {
                if (parse_bool(val, &b)) cfg->usifac = b;
                else { fprintf(stderr, "1984.conf:%d: usifac must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "usifac_backend")) {
                if (strcmp(val, "pty") && strcmp(val, "tcp"))
                    fprintf(stderr, "1984.conf:%d: usifac_backend must be 'pty' or 'tcp' (got '%s')\n", lineno, val);
                else
                    snprintf(cfg->usifac_backend, sizeof(cfg->usifac_backend), "%s", val);
            } else if (!strcmp(key, "usifac_tcp_port")) {
                int p = atoi(val);
                if (p > 0 && p < 65536) cfg->usifac_tcp_port = p;
                else { fprintf(stderr, "1984.conf:%d: usifac_tcp_port must be 1..65535\n", lineno); rc = -1; }
            } else if (!strcmp(key, "usifac_pty_link")) {
                snprintf(cfg->usifac_pty_link, sizeof(cfg->usifac_pty_link), "%s", val);
            } else if (!strcmp(key, "perryfi")) {
                if (parse_bool(val, &b)) cfg->perryfi = b;
                else { fprintf(stderr, "1984.conf:%d: perryfi must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "ulifac")) {
                /* Legacy key — renamed to 'usifac' in v0.4.8. Accept and warn. */
                static bool warned = false;
                if (!warned) {
                    fprintf(stderr, "1984.conf:%d: 'ulifac' has been renamed to 'usifac' — please update your config\n", lineno);
                    warned = true;
                }
                if (parse_bool(val, &b)) cfg->usifac = b;
                else { fprintf(stderr, "1984.conf:%d: ulifac must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "net4cpc")) {
                if (parse_bool(val, &b)) cfg->net4cpc = b;
                else { fprintf(stderr, "1984.conf:%d: net4cpc must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "net4cpc_tap")) {
                if (parse_bool(val, &b)) cfg->net4cpc_tap = b;
                else { fprintf(stderr, "1984.conf:%d: net4cpc_tap must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "net4cpc_tap_host_ip")) {
                snprintf(cfg->net4cpc_tap_host_ip, sizeof(cfg->net4cpc_tap_host_ip), "%s", val);
            } else if (!strcmp(key, "net4cpc_tap_netmask")) {
                snprintf(cfg->net4cpc_tap_netmask, sizeof(cfg->net4cpc_tap_netmask), "%s", val);
            } else if (!strcmp(key, "net4cpc_tap_lease_start")) {
                snprintf(cfg->net4cpc_tap_lease_start, sizeof(cfg->net4cpc_tap_lease_start), "%s", val);
            } else if (!strcmp(key, "net4cpc_tap_lease_end")) {
                snprintf(cfg->net4cpc_tap_lease_end, sizeof(cfg->net4cpc_tap_lease_end), "%s", val);
            } else if (!strcmp(key, "rtc")) {
                if (parse_bool(val, &b)) cfg->rtc = b;
                else { fprintf(stderr, "1984.conf:%d: rtc must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "symbiface_ide")) {
                if (parse_bool(val, &b)) cfg->symbiface_ide = b;
                else { fprintf(stderr, "1984.conf:%d: symbiface_ide must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "ide_image")) {
                if (val[0]) expand_path(val, cfg->ide_image, sizeof(cfg->ide_image));
            } else if (!strcmp(key, "symbiface_mouse")) {
                if (parse_bool(val, &b)) cfg->symbiface_mouse = b;
                else { fprintf(stderr, "1984.conf:%d: symbiface_mouse must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "symbnet")) {
                if (parse_bool(val, &b)) cfg->symbnet = b;
                else { fprintf(stderr, "1984.conf:%d: symbnet must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "albireo")) {
                if (parse_bool(val, &b)) cfg->albireo = b;
                else { fprintf(stderr, "1984.conf:%d: albireo must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "albireo_image")) {
                if (val[0]) expand_path(val, cfg->albireo_image, sizeof(cfg->albireo_image));
            } else if (!strcmp(key, "albireo_mouse")) {
                if (parse_bool(val, &b)) cfg->albireo_mouse = b;
                else { fprintf(stderr, "1984.conf:%d: albireo_mouse must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "albireo_disable_disk_read")) {
                if (parse_bool(val, &b)) cfg->albireo_disable_disk_read = b;
                else { fprintf(stderr, "1984.conf:%d: albireo_disable_disk_read must be true/false\n", lineno); rc = -1; }
            }
        } else if (!strcmp(section, "printer")) {
            if (!strcmp(key, "pdf_printer")) {
                bool b;
                if (parse_bool(val, &b)) cfg->pdf_printer = b;
                else { fprintf(stderr, "1984.conf:%d: pdf_printer must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "pdf_printer_dir")) {
                if (val[0]) expand_path(val, cfg->pdf_printer_dir, sizeof(cfg->pdf_printer_dir));
            } else if (!strcmp(key, "print_sink")) {
                if (!strcasecmp(val, "pdf"))            cfg->print_sink = PRINTER_SINK_PDF;
                else if (!strcasecmp(val, "real"))      cfg->print_sink = PRINTER_SINK_REAL_PRINTER;
                else { fprintf(stderr, "1984.conf:%d: print_sink must be pdf/real\n", lineno); rc = -1; }
            }
        } else if (!strcmp(section, "display")) {
            if (!strcmp(key, "scale")) {
                int sc = atoi(val);
                if (sc >= 1 && sc <= 4) cfg->scale = sc;
                else { fprintf(stderr, "1984.conf:%d: scale must be 1, 2, 3, or 4\n", lineno); rc = -1; }
            } else if (!strcmp(key, "fullscreen")) {
                bool b;
                if (parse_bool(val, &b)) cfg->fullscreen = b;
                else { fprintf(stderr, "1984.conf:%d: fullscreen must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "fullscreen_smoothing")) {
                bool b;
                if (parse_bool(val, &b)) cfg->fullscreen_smoothing = b;
                else { fprintf(stderr, "1984.conf:%d: fullscreen_smoothing must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "real_crt")) {
                bool b;
                if (parse_bool(val, &b)) cfg->real_crt = b;
                else { fprintf(stderr, "1984.conf:%d: real_crt must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "crt_scanlines")) {
                int v = atoi(val);
                if (v >= 0 && v <= 95) cfg->crt_scanlines = v;
                else { fprintf(stderr, "1984.conf:%d: crt_scanlines must be 0..95\n", lineno); rc = -1; }
            } else if (!strcmp(key, "crt_brightness")) {
                int v = atoi(val);
                if (v >= 50 && v <= 100) cfg->crt_brightness = v;
                else { fprintf(stderr, "1984.conf:%d: crt_brightness must be 50..100\n", lineno); rc = -1; }
            } else if (!strcmp(key, "crt_contrast")) {
                int v = atoi(val);
                if (v >= 50 && v <= 150) cfg->crt_contrast = v;
                else { fprintf(stderr, "1984.conf:%d: crt_contrast must be 50..150\n", lineno); rc = -1; }
            } else if (!strcmp(key, "monochrome")) {
                MonoMode m;
                if (parse_mono(val, &m)) cfg->monochrome = m;
                else { fprintf(stderr, "1984.conf:%d: monochrome must be off/green/amber/white\n", lineno); rc = -1; }
            }
        } else if (!strcmp(section, "audio")) {
            if (!strcmp(key, "audio_volume")) {
                int v = atoi(val);
                if (v >= 0 && v <= 100) cfg->audio_volume = v;
                else { fprintf(stderr, "1984.conf:%d: audio_volume must be 0..100\n", lineno); rc = -1; }
            } else if (!strcmp(key, "audio_stereo_sep")) {
                int v = atoi(val);
                if (v >= 0 && v <= 255) cfg->audio_stereo_sep = v;
                else { fprintf(stderr, "1984.conf:%d: audio_stereo_sep must be 0..255\n", lineno); rc = -1; }
            }
        } else if (!strcmp(section, "advanced")) {
            if (!strcmp(key, "tinker")) {
                bool b;
                if (parse_bool(val, &b)) cfg->tinker = b;
                else { fprintf(stderr, "1984.conf:%d: tinker must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "debug")) {
                bool b;
                if (parse_bool(val, &b)) cfg->debug = b;
                else { fprintf(stderr, "1984.conf:%d: debug must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "last_dir")) {
                if (val[0]) expand_path(val, cfg->last_dir, sizeof(cfg->last_dir));
            }
        }
    }

    fclose(f);

    /* Restore defaults for any fields left invalid/empty by a corrupt config */
    if (!cfg->rom_os[0])
        config_default_os(cfg->model, cfg->rom_os, sizeof(cfg->rom_os));
    if (!cfg->rom_basic[0])
        config_default_basic(cfg->model, cfg->rom_basic, sizeof(cfg->rom_basic));
    if (cfg->memory_kb == 0)
        cfg->memory_kb = 128;

    /* M4ROM is incompatible with UNIDOS/Albireo tooling and with the
     * Cyboard RTC. Older configs may have both enabled — keep M4 and
     * disable the conflicting peripherals so the runtime starts in a
     * clean scenario that matches the overlay's mutual-exclusion rule. */
    if (cfg->m4) {
        if (cfg->rtc)     cfg->rtc = false;
        if (cfg->albireo) { cfg->albireo = false; cfg->albireo_image[0] = '\0'; }
    }

    return rc;
}

int config_save(const Config *cfg) {
    char cdir[CONFIG_PATH_MAX];
    if (config_dir(cdir, sizeof(cdir), true) != 0) {
        fprintf(stderr, "1984: cannot resolve config directory "
                        "(neither $HOME nor %%APPDATA%%/%%USERPROFILE%% set)\n");
        return -1;
    }

    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/1984.conf", cdir);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "1984: could not save config to %s\n", path);
        return -1;
    }

    fprintf(f,
        "# 1984 CPC Emulator — configuration file\n\n"
        "[machine]\n"
        "model=%s\n"
        "memory=%d\n\n"
        "[roms]\n"
        "os=%s\n"
        "basic=%s\n"
        "amsdos=%s\n",
        cfg->model == MODEL_464 ? "464" :
            (cfg->model == MODEL_664 ? "664" : "6128"),
        cfg->memory_kb,
        cfg->rom_os,
        cfg->rom_basic,
        cfg->rom_amsdos);

    fprintf(f, "\n[expansion_roms]\n");
    for (int i = 0; i < ROM_EXT_COUNT; i++) {
        /* M4_ROM_SLOT is managed automatically when M4 is enabled — don't persist it */
        if (i == M4_ROM_SLOT && cfg->m4) continue;
        if (cfg->rom_ext[i][0])
            fprintf(f, "slot_%d=%s\n", i, cfg->rom_ext[i]);
    }

    /* [board:NAME] sections — per-board conf templates: ROM slot
     * paths the board needs, plus a cached disk-image path for boards
     * that have one (m4 SD card, albireo USB drive, cyboard IDE).
     * Toggling the board on populates the live cfg fields from here;
     * toggling off keeps the templates so the next enable doesn't
     * re-prompt. See config_apply_boards() and the overlay activate
     * handlers in src/overlay.c. */
    for (int b = 0; b < CONFIG_BOARDS_COUNT; b++) {
        const char *board = CONFIG_BOARDS[b];
        char (*tbl)[CONFIG_PATH_MAX] = config_board_slots((Config *)cfg, board);
        char *img = config_board_image((Config *)cfg, board);
        if (!tbl && !img) continue;
        bool any_slot = false;
        for (int i = 0; tbl && i < ROM_EXT_COUNT; i++)
            if (tbl[i][0]) { any_slot = true; break; }
        bool any_img = img && img[0];
        if (!any_slot && !any_img) continue;
        fprintf(f, "\n[board:%s]\n", board);
        for (int i = 0; tbl && i < ROM_EXT_COUNT; i++) {
            if (tbl[i][0])
                fprintf(f, "slot_%d=%s\n", i, tbl[i]);
        }
        if (any_img)
            fprintf(f, "image=%s\n", img);
    }

    fprintf(f,
        "\n[storage]\n"
        "drive_a=%s\n"
        "drive_b=%s\n"
        "tape=%s\n"
        "external_tape=%s\n\n"
        "[hardware]\n"
        "mx4=%s\n"
        "rom_board=%s\n"
        "dd1=%s\n"
        "m4=%s\n"
        "m4_path=%s\n"
        "m4_image=%s\n"
        "usifac=%s\n"
        "usifac_backend=%s\n"
        "usifac_tcp_port=%d\n"
        "usifac_pty_link=%s\n"
        "perryfi=%s\n"
        "net4cpc=%s\n"
        "net4cpc_tap=%s\n"
        "net4cpc_tap_host_ip=%s\n"
        "net4cpc_tap_netmask=%s\n"
        "net4cpc_tap_lease_start=%s\n"
        "net4cpc_tap_lease_end=%s\n"
        "rtc=%s\n"
        "symbiface_ide=%s\n"
        "ide_image=%s\n"
        "symbiface_mouse=%s\n"
        "symbnet=%s\n"
        "albireo=%s\n"
        "albireo_image=%s\n"
        "albireo_mouse=%s\n"
        "albireo_disable_disk_read=%s\n\n"
        "[printer]\n"
        "pdf_printer=%s\n"
        "pdf_printer_dir=%s\n"
        "print_sink=%s\n\n"
        "[display]\n"
        "scale=%d\n"
        "fullscreen=%s\n"
        "fullscreen_smoothing=%s\n"
        "real_crt=%s\n"
        "crt_scanlines=%d\n"
        "crt_brightness=%d\n"
        "crt_contrast=%d\n"
        "monochrome=%s\n\n"
        "[audio]\n"
        "audio_volume=%d\n"
        "audio_stereo_sep=%d\n\n"
        "[advanced]\n"
        "tinker=%s\n"
        "debug=%s\n"
        "last_dir=%s\n",
        cfg->disk_a,
        cfg->disk_b,
        cfg->tape,
        cfg->external_tape ? "true" : "false",
        cfg->mx4        ? "true" : "false",
        cfg->rom_board  ? "true" : "false",
        cfg->dd1        ? "true" : "false",
        cfg->m4      ? "true" : "false",
        cfg->m4_path,
        cfg->m4_image,
        cfg->usifac           ? "true" : "false",
        cfg->usifac_backend,
        cfg->usifac_tcp_port,
        cfg->usifac_pty_link,
        cfg->perryfi          ? "true" : "false",
        cfg->net4cpc          ? "true" : "false",
        cfg->net4cpc_tap      ? "true" : "false",
        cfg->net4cpc_tap_host_ip,
        cfg->net4cpc_tap_netmask,
        cfg->net4cpc_tap_lease_start,
        cfg->net4cpc_tap_lease_end,
        cfg->rtc              ? "true" : "false",
        cfg->symbiface_ide    ? "true" : "false",
        cfg->ide_image,
        cfg->symbiface_mouse  ? "true" : "false",
        cfg->symbnet          ? "true" : "false",
        cfg->albireo          ? "true" : "false",
        cfg->albireo_image,
        cfg->albireo_mouse    ? "true" : "false",
        cfg->albireo_disable_disk_read ? "true" : "false",
        cfg->pdf_printer ? "true" : "false",
        cfg->pdf_printer_dir,
        cfg->print_sink == PRINTER_SINK_REAL_PRINTER ? "real" : "pdf",
        cfg->scale,
        cfg->fullscreen ? "true" : "false",
        cfg->fullscreen_smoothing ? "true" : "false",
        cfg->real_crt ? "true" : "false",
        cfg->crt_scanlines,
        cfg->crt_brightness,
        cfg->crt_contrast,
        mono_to_str(cfg->monochrome),
        cfg->audio_volume,
        cfg->audio_stereo_sep,
        cfg->tinker     ? "true" : "false",
        cfg->debug      ? "true" : "false",
        cfg->last_dir
    );

    fclose(f);
    return 0;
}

void config_set_model(Config *cfg, CpcModel model) {
    cfg->model = model;
    if (model == MODEL_464) {
        cfg->memory_kb = 64;
        cfg->dd1       = false;
        rom_cfg_path(ROM_FILE_OS_464,    cfg->rom_os,    sizeof(cfg->rom_os));
        rom_cfg_path(ROM_FILE_BASIC_464, cfg->rom_basic, sizeof(cfg->rom_basic));
        cfg->rom_amsdos[0] = '\0';   /* 464 has no built-in AMSDOS; DD1 adds it */
    } else if (model == MODEL_664) {
        cfg->memory_kb = 64;
        cfg->dd1       = false;      /* 664 has built-in FDC; DD1 toggle N/A */
        rom_cfg_path(ROM_FILE_OS_664,     cfg->rom_os,     sizeof(cfg->rom_os));
        rom_cfg_path(ROM_FILE_BASIC_664,  cfg->rom_basic,  sizeof(cfg->rom_basic));
        rom_cfg_path(ROM_FILE_AMSDOS_664, cfg->rom_amsdos, sizeof(cfg->rom_amsdos));
    } else {
        cfg->memory_kb = 128;
        cfg->dd1       = false;      /* 6128 has built-in FDC; DD1 not applicable */
        rom_cfg_path(ROM_FILE_OS_6128,    cfg->rom_os,     sizeof(cfg->rom_os));
        rom_cfg_path(ROM_FILE_BASIC_6128, cfg->rom_basic,  sizeof(cfg->rom_basic));
        rom_cfg_path(ROM_FILE_AMSDOS,     cfg->rom_amsdos, sizeof(cfg->rom_amsdos));
    }
}

void config_apply_dd1(Config *cfg, bool enabled) {
    cfg->dd1 = enabled;
    if (enabled)
        rom_cfg_path(ROM_FILE_AMSDOS, cfg->rom_amsdos, sizeof(cfg->rom_amsdos));
    else
        cfg->rom_amsdos[0] = '\0';
}

void config_default_os(CpcModel model, char *out, size_t sz) {
    const char *file = ROM_FILE_OS_6128;
    if (model == MODEL_464) file = ROM_FILE_OS_464;
    else if (model == MODEL_664) file = ROM_FILE_OS_664;
    rom_cfg_path(file, out, sz);
}

void config_default_basic(CpcModel model, char *out, size_t sz) {
    const char *file = ROM_FILE_BASIC_6128;
    if (model == MODEL_464) file = ROM_FILE_BASIC_464;
    else if (model == MODEL_664) file = ROM_FILE_BASIC_664;
    rom_cfg_path(file, out, sz);
}

void config_default_amsdos(CpcModel model, char *out, size_t sz) {
    rom_cfg_path(model == MODEL_664 ? ROM_FILE_AMSDOS_664 : ROM_FILE_AMSDOS,
                 out, sz);
}

void config_default_m4rom(char *out, size_t sz) {
    rom_cfg_path(ROM_FILE_M4ROM, out, sz);
}

void config_default_diag(char *out, size_t sz) {
    rom_cfg_path(ROM_FILE_DIAG, out, sz);
}

/* ---------------------------------------------------------------
 * Per-board ROM tagging — see Config.rom_ext_boards in config.h.
 * --------------------------------------------------------------- */

char (*config_board_slots(Config *cfg, const char *board))[CONFIG_PATH_MAX] {
    if (!cfg || !board) return NULL;
    if (!strcmp(board, "m4"))      return cfg->board_m4_slot;
    if (!strcmp(board, "albireo")) return cfg->board_albireo_slot;
    if (!strcmp(board, "cyboard")) return cfg->board_cyboard_slot;
    return NULL;
}

char *config_board_image(Config *cfg, const char *board) {
    if (!cfg || !board) return NULL;
    if (!strcmp(board, "m4"))      return cfg->board_m4_image;
    if (!strcmp(board, "albireo")) return cfg->board_albireo_image;
    if (!strcmp(board, "cyboard")) return cfg->board_cyboard_image;
    return NULL;
}

bool config_boards_contains(const char *csv, const char *board) {
    if (!csv || !board || !csv[0]) return false;
    size_t bl = strlen(board);
    const char *p = csv;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        /* trim trailing spaces */
        while (len > 0 && start[len - 1] == ' ') len--;
        if (len == bl && strncmp(start, board, bl) == 0) return true;
    }
    return false;
}

void config_normalize_boards(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t out_len = 0;
    const char *p = in;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
        if (!len) continue;
        /* lowercase into a stack buffer for compare */
        char tok[32];
        if (len >= sizeof(tok)) { /* too long → reject */
            fprintf(stderr, "1984: ROM board tag '%.*s' too long, ignored\n", (int)len, start);
            continue;
        }
        for (size_t i = 0; i < len; i++)
            tok[i] = (start[i] >= 'A' && start[i] <= 'Z') ? (char)(start[i] + 32) : start[i];
        tok[len] = '\0';
        /* whitelist check */
        bool known = false;
        for (int b = 0; b < CONFIG_BOARDS_COUNT; b++)
            if (!strcmp(tok, CONFIG_BOARDS[b])) { known = true; break; }
        if (!known) {
            fprintf(stderr, "1984: unknown ROM board tag '%s' (allowed: m4, albireo, cyboard)\n", tok);
            continue;
        }
        /* dedupe */
        if (config_boards_contains(out, tok)) continue;
        size_t need = out_len + (out_len ? 1 : 0) + strlen(tok) + 1;
        if (need > out_sz) {
            fprintf(stderr, "1984: ROM board tag list overflow, '%s' dropped\n", tok);
            continue;
        }
        if (out_len) out[out_len++] = ',';
        strcpy(out + out_len, tok);
        out_len += strlen(tok);
    }
}

/* Look up the path for `slot` under `board`. NULL if board unknown
 * or slot has no template path under that board. */
static const char *board_template_path(Config *cfg, const char *board, int slot) {
    char (*tbl)[CONFIG_PATH_MAX] = config_board_slots(cfg, board);
    if (!tbl) return NULL;
    if (slot < 0 || slot >= ROM_EXT_COUNT) return NULL;
    return tbl[slot][0] ? tbl[slot] : NULL;
}

/* Maps a board name to whether its hardware bool is currently on. */
static bool board_is_active(const Config *cfg, const char *board) {
    if (!strcmp(board, "m4"))      return cfg->m4;
    if (!strcmp(board, "albireo")) return cfg->albireo;
    if (!strcmp(board, "cyboard")) return cfg->symbiface_ide;
    return false;
}

/* Maps a board name to the live cfg disk-image field (NOT the cached
 * board_*_image), or NULL if board has no image concept. */
static char *board_live_image(Config *cfg, const char *board) {
    if (!strcmp(board, "m4"))      return cfg->m4_image;
    if (!strcmp(board, "albireo")) return cfg->albireo_image;
    if (!strcmp(board, "cyboard")) return cfg->ide_image;
    return NULL;
}

int config_apply_boards(Config *cfg) {
    int changed = 0;
    /* Image sync: for each ACTIVE board, copy its cached image into
     * the live cfg field if the live field is empty (so a user-pinned
     * image isn't overwritten). For each INACTIVE board, do nothing
     * to the live field — disabling a board doesn't blow away its
     * image unless the user explicitly clears it via Del. */
    for (int b = 0; b < CONFIG_BOARDS_COUNT; b++) {
        const char *board = CONFIG_BOARDS[b];
        if (!board_is_active(cfg, board)) continue;
        char *cached = config_board_image(cfg, board);
        char *live   = board_live_image(cfg, board);
        if (cached && live && cached[0] && !live[0]) {
            snprintf(live, CONFIG_PATH_MAX, "%s", cached);
            changed++;
        }
    }
    for (int slot = 0; slot < ROM_EXT_COUNT; slot++) {
        const char *csv = cfg->rom_ext_boards[slot];
        if (!csv[0]) continue;  /* user-pinned; leave alone */
        /* Find the last (alphabetically: m4 < albireo < cyboard) active
         * board for this slot. Last-wins on multi-board conflicts; warn
         * if more than one active claims this slot. */
        const char *winner = NULL;
        const char *winner_path = NULL;
        for (int b = 0; b < CONFIG_BOARDS_COUNT; b++) {
            const char *board = CONFIG_BOARDS[b];
            if (!config_boards_contains(csv, board)) continue;
            if (!board_is_active(cfg, board)) continue;
            const char *p = board_template_path(cfg, board, slot);
            if (!p) continue;
            if (winner_path && strcmp(winner_path, p) != 0)
                fprintf(stderr, "1984: slot %d conflict — board '%s' wants '%s', "
                                "overrides earlier '%s' from '%s'\n",
                        slot, board, p, winner_path, winner ? winner : "?");
            winner = board;
            winner_path = p;
        }
        if (winner_path) {
            if (strcmp(cfg->rom_ext[slot], winner_path) != 0) {
                snprintf(cfg->rom_ext[slot], sizeof(cfg->rom_ext[slot]),
                         "%s", winner_path);
                changed++;
            }
        } else {
            /* No active board for this slot — clear it. */
            if (cfg->rom_ext[slot][0]) {
                cfg->rom_ext[slot][0] = '\0';
                changed++;
            }
        }
    }
    return changed;
}
