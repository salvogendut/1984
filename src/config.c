#include "config.h"
#include "m4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

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
    snprintf(dir, sizeof(dir), "%s/.config/1984", home);
    mkdir(dir, 0755);   /* no-op if already exists */

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
        "os=~/.config/1984/roms/OS_6128.ROM\n"
        "basic=~/.config/1984/roms/BASIC_1.1.ROM\n"
        "amsdos=~/.config/1984/roms/AMSDOS.ROM\n"
        "\n"
        "[expansion_roms]\n"
        "# Load extra ROMs into upper ROM slots 0-31.\n"
        "# slot_7 is AMSDOS by default; leave entries empty to use defaults.\n"
        "# Example: slot_5=/path/to/TOOLKIT.ROM\n"
        "\n"
        "[storage]\n"
        "# Paths to .dsk floppy images (leave empty for no disk)\n"
        "drive_a=\n"
        "drive_b=\n"
        "\n"
        "[hardware]\n"
        "# mx4: MX4 expansion bus — when false, all extension peripherals\n"
        "# (M4, Net4CPC, RTC, SYMBiFACE, Albireo, …) are disconnected.\n"
        "mx4=true\n"
        "# dd1: CPC 464 only — DDI-1 floppy interface (enables drives + AMSDOS)\n"
        "dd1=false\n"
        "# Optional expansion hardware (not yet implemented)\n"
        "m4=false\n"
        "m4_path=\n"
        "m4_image=\n"
        "ulifac=false\n"
        "net4cpc=false\n"
        "rtc=false\n"
        "symbiface_ide=false\n"
        "ide_image=\n"
        "symbiface_mouse=false\n"
        "symbnet=false\n"
        "albireo=false\n"
        "albireo_image=\n"
        "\n"
        "[display]\n"
        "# Window scale factor: 1, 2, or 3\n"
        "scale=2\n"
        "# Start in fullscreen mode: true or false\n"
        "fullscreen=false\n"
    );

    fclose(f);
    fprintf(stderr, "1984: created default config at %s\n", path);
}

int config_load(Config *cfg) {
    config_defaults(cfg);

    const char *home = getenv("HOME");
    if (!home) return 0;

    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/1984/1984.conf", home);

    FILE *f = fopen(path, "r");
    if (!f) {
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
        } else if (!strcmp(section, "storage")) {
            if (!strcmp(key, "drive_a"))
                expand_path(val, cfg->disk_a, sizeof(cfg->disk_a));
            else if (!strcmp(key, "drive_b"))
                expand_path(val, cfg->disk_b, sizeof(cfg->disk_b));
        } else if (!strcmp(section, "hardware")) {
            bool b;
            if (!strcmp(key, "mx4")) {
                if (parse_bool(val, &b)) cfg->mx4 = b;
                else { fprintf(stderr, "1984.conf:%d: mx4 must be true/false\n", lineno); rc = -1; }
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

    return rc;
}

int config_save(const Config *cfg) {
    const char *home = getenv("HOME");
    if (!home) return -1;

    char path[CONFIG_PATH_MAX];
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

    fprintf(f,
        "\n[storage]\n"
        "drive_a=%s\n"
        "drive_b=%s\n\n"
        "[hardware]\n"
        "mx4=%s\n"
        "dd1=%s\n"
        "m4=%s\n"
        "m4_path=%s\n"
        "m4_image=%s\n"
        "ulifac=%s\n"
        "net4cpc=%s\n"
        "rtc=%s\n"
        "symbiface_ide=%s\n"
        "ide_image=%s\n"
        "symbiface_mouse=%s\n"
        "symbnet=%s\n"
        "albireo=%s\n"
        "albireo_image=%s\n\n"
        "[display]\n"
        "scale=%d\n"
        "fullscreen=%s\n",
        cfg->disk_a,
        cfg->disk_b,
        cfg->mx4     ? "true" : "false",
        cfg->dd1     ? "true" : "false",
        cfg->m4      ? "true" : "false",
        cfg->m4_path,
        cfg->m4_image,
        cfg->ulifac  ? "true" : "false",
        cfg->net4cpc          ? "true" : "false",
        cfg->rtc              ? "true" : "false",
        cfg->symbiface_ide    ? "true" : "false",
        cfg->ide_image,
        cfg->symbiface_mouse  ? "true" : "false",
        cfg->symbnet          ? "true" : "false",
        cfg->albireo          ? "true" : "false",
        cfg->albireo_image,
        cfg->scale,
        cfg->fullscreen ? "true" : "false"
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
