#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

void config_set_model(Config *cfg, CpcModel model);  /* defined below */

#define DEFAULT_ROM_OS_464     "roms/OS_464.ROM"
#define DEFAULT_ROM_BASIC_464  "roms/BASIC_1.0.ROM"
#define DEFAULT_ROM_OS_6128    "roms/OS_6128.ROM"
#define DEFAULT_ROM_BASIC_6128 "roms/BASIC_1.1.ROM"
#define DEFAULT_ROM_AMSDOS     "roms/AMSDOS.ROM"

/* Keep backward-compat aliases used by the defaults/restore paths */
#define DEFAULT_ROM_OS    DEFAULT_ROM_OS_6128
#define DEFAULT_ROM_BASIC DEFAULT_ROM_BASIC_6128

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->scale     = 2;
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
        "# dd1: CPC 464 only — DDI-1 floppy interface (enables drives + AMSDOS)\n"
        "dd1=false\n"
        "# Optional expansion hardware (not yet implemented)\n"
        "m4=false\n"
        "ulifac=false\n"
        "net4cpc=false\n"
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
                if (kb == 64 || kb == 128) cfg->memory_kb = kb;
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
            if (!strcmp(key, "dd1")) {
                if (parse_bool(val, &b)) cfg->dd1 = b;
                else { fprintf(stderr, "1984.conf:%d: dd1 must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "m4")) {
                if (parse_bool(val, &b)) cfg->m4 = b;
                else { fprintf(stderr, "1984.conf:%d: m4 must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "ulifac")) {
                if (parse_bool(val, &b)) cfg->ulifac = b;
                else { fprintf(stderr, "1984.conf:%d: ulifac must be true/false\n", lineno); rc = -1; }
            } else if (!strcmp(key, "net4cpc")) {
                if (parse_bool(val, &b)) cfg->net4cpc = b;
                else { fprintf(stderr, "1984.conf:%d: net4cpc must be true/false\n", lineno); rc = -1; }
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
        snprintf(cfg->rom_os, sizeof(cfg->rom_os), "%s", DEFAULT_ROM_OS);
    if (!cfg->rom_basic[0])
        snprintf(cfg->rom_basic, sizeof(cfg->rom_basic), "%s", DEFAULT_ROM_BASIC);
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
        if (cfg->rom_ext[i][0])
            fprintf(f, "slot_%d=%s\n", i, cfg->rom_ext[i]);
    }

    fprintf(f,
        "\n[storage]\n"
        "drive_a=%s\n"
        "drive_b=%s\n\n"
        "[hardware]\n"
        "dd1=%s\n"
        "m4=%s\n"
        "ulifac=%s\n"
        "net4cpc=%s\n\n"
        "[display]\n"
        "scale=%d\n"
        "fullscreen=%s\n",
        cfg->disk_a,
        cfg->disk_b,
        cfg->dd1     ? "true" : "false",
        cfg->m4      ? "true" : "false",
        cfg->ulifac  ? "true" : "false",
        cfg->net4cpc ? "true" : "false",
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
        snprintf(cfg->rom_os,    sizeof(cfg->rom_os),    "%s", DEFAULT_ROM_OS_464);
        snprintf(cfg->rom_basic, sizeof(cfg->rom_basic), "%s", DEFAULT_ROM_BASIC_464);
        cfg->rom_amsdos[0] = '\0';   /* 464 has no built-in AMSDOS; DD1 adds it */
    } else {
        cfg->memory_kb = 128;
        cfg->dd1       = false;      /* 6128 has built-in FDC; DD1 not applicable */
        snprintf(cfg->rom_os,     sizeof(cfg->rom_os),     "%s", DEFAULT_ROM_OS_6128);
        snprintf(cfg->rom_basic,  sizeof(cfg->rom_basic),  "%s", DEFAULT_ROM_BASIC_6128);
        snprintf(cfg->rom_amsdos, sizeof(cfg->rom_amsdos), "%s", DEFAULT_ROM_AMSDOS);
    }
}

void config_apply_dd1(Config *cfg, bool enabled) {
    cfg->dd1 = enabled;
    if (enabled)
        snprintf(cfg->rom_amsdos, sizeof(cfg->rom_amsdos), "%s", DEFAULT_ROM_AMSDOS);
    else
        cfg->rom_amsdos[0] = '\0';
}

void config_default_os(CpcModel model, char *out, size_t sz) {
    snprintf(out, sz, "%s",
        model == MODEL_464 ? DEFAULT_ROM_OS_464 : DEFAULT_ROM_OS_6128);
}

void config_default_basic(CpcModel model, char *out, size_t sz) {
    snprintf(out, sz, "%s",
        model == MODEL_464 ? DEFAULT_ROM_BASIC_464 : DEFAULT_ROM_BASIC_6128);
}

void config_default_amsdos(char *out, size_t sz) {
    snprintf(out, sz, "%s", DEFAULT_ROM_AMSDOS);
}
