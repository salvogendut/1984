#include "config.h"
#include "m4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include "compat_win.h"   /* mkdir(mode) shim on Windows */

void config_set_model(Config *cfg, CpcModel model);  /* defined below */

#define ROM_FILE_OS_464     "OS_464.ROM"
#define ROM_FILE_BASIC_464  "BASIC_1.0.ROM"
#define ROM_FILE_OS_6128    "OS_6128.ROM"
#define ROM_FILE_BASIC_6128 "BASIC_1.1.ROM"
#define ROM_FILE_AMSDOS     "AMSDOS.ROM"
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
    const char *home = getenv("HOME");

    user[0] = '\0';
    if (home)
        snprintf(user, sizeof(user), "%s/.config/1984/roms/%s", home, file);

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
    cfg->scale     = 2;
    cfg->mx4       = true;   /* expansion bus connected by default */
    cfg->rom_board = true;   /* ROM Board fitted by default */
    cfg->fullscreen_smoothing = true;  /* preserve historic linear-scale behaviour */
    cfg->tinker    = false;
    cfg->debug     = false;
    snprintf(cfg->net4cpc_tap_host_ip,    sizeof(cfg->net4cpc_tap_host_ip),    "10.0.0.1");
    snprintf(cfg->net4cpc_tap_netmask,    sizeof(cfg->net4cpc_tap_netmask),    "255.255.255.0");
    snprintf(cfg->net4cpc_tap_lease_start, sizeof(cfg->net4cpc_tap_lease_start), "10.0.0.100");
    snprintf(cfg->net4cpc_tap_lease_end,  sizeof(cfg->net4cpc_tap_lease_end),  "10.0.0.150");
    config_set_model(cfg, MODEL_6128);  /* sets model, memory, OS, BASIC, AMSDOS */
}

/* Expand a leading ~ to the home directory. Result written into out[size]. */
static void expand_path(const char *in, char *out, size_t size) {
    if (in[0] == '~') {
        const char *home = getenv("HOME");
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

static bool parse_bool(const char *val, bool *out) {
    if (!strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes")) {
        *out = true; return true;
    }
    if (!strcmp(val, "false") || !strcmp(val, "0") || !strcmp(val, "no")) {
        *out = false; return true;
    }
    return false;
}

static void config_create_default(const char *path, const char *home) {
    char dir[CONFIG_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    mkdir(dir, 0755);   /* parent may not exist on fresh accounts (e.g. Haiku) */
    snprintf(dir, sizeof(dir), "%s/.config/1984", home);
    mkdir(dir, 0755);   /* no-op if already exists */

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
        "# CPC model: 464 or 6128\n"
        "model=6128\n"
        "# RAM size in KB: 64 (CPC 464) or 128 (CPC 6128)\n"
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
        "ulifac=false\n"
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
        const char *home = getenv("HOME");
        if (!home) return 0;
        snprintf(path, sizeof(path), "%s/.config/1984/1984.conf", home);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        if (using_override) {
            fprintf(stderr, "1984: --config: cannot open '%s'\n", path);
            return -1;
        }
        const char *home = getenv("HOME");
        config_create_default(path, home);
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
            } else if (!strcmp(key, "ulifac")) {
                if (parse_bool(val, &b)) cfg->ulifac = b;
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
        } else if (!strcmp(section, "display")) {
            if (!strcmp(key, "scale")) {
                int sc = atoi(val);
                if (sc >= 1 && sc <= 3) cfg->scale = sc;
                else { fprintf(stderr, "1984.conf:%d: scale must be 1, 2, or 3\n", lineno); rc = -1; }
            } else if (!strcmp(key, "fullscreen")) {
                bool b;
                if (parse_bool(val, &b)) cfg->fullscreen = b;
                else { fprintf(stderr, "1984.conf:%d: fullscreen must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "fullscreen_smoothing")) {
                bool b;
                if (parse_bool(val, &b)) cfg->fullscreen_smoothing = b;
                else { fprintf(stderr, "1984.conf:%d: fullscreen_smoothing must be true/false\n", lineno); rc = -1; }
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
            }
        }
    }

    fclose(f);

    /* Restore defaults for any fields left invalid/empty by a corrupt config */
    if (!cfg->rom_os[0])
        rom_cfg_path(cfg->model == MODEL_464 ? ROM_FILE_OS_464 : ROM_FILE_OS_6128,
                     cfg->rom_os, sizeof(cfg->rom_os));
    if (!cfg->rom_basic[0])
        rom_cfg_path(cfg->model == MODEL_464 ? ROM_FILE_BASIC_464 : ROM_FILE_BASIC_6128,
                     cfg->rom_basic, sizeof(cfg->rom_basic));
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
    const char *home = getenv("HOME");
    if (!home) return -1;

    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.config/1984", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.config/1984/1984.conf", home);

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
        cfg->model == MODEL_464 ? "464" : "6128",
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
        "ulifac=%s\n"
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
        "[display]\n"
        "scale=%d\n"
        "fullscreen=%s\n"
        "fullscreen_smoothing=%s\n\n"
        "[advanced]\n"
        "tinker=%s\n"
        "debug=%s\n",
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
        cfg->ulifac  ? "true" : "false",
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
        cfg->scale,
        cfg->fullscreen ? "true" : "false",
        cfg->fullscreen_smoothing ? "true" : "false",
        cfg->tinker     ? "true" : "false",
        cfg->debug      ? "true" : "false"
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
    rom_cfg_path(model == MODEL_464 ? ROM_FILE_OS_464 : ROM_FILE_OS_6128, out, sz);
}

void config_default_basic(CpcModel model, char *out, size_t sz) {
    rom_cfg_path(model == MODEL_464 ? ROM_FILE_BASIC_464 : ROM_FILE_BASIC_6128, out, sz);
}

void config_default_amsdos(char *out, size_t sz) {
    rom_cfg_path(ROM_FILE_AMSDOS, out, sz);
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
