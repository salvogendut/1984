#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#define DEFAULT_ROM_OS    "roms/OS_6128.ROM"
#define DEFAULT_ROM_BASIC "roms/BASIC_1.1.ROM"

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->model     = MODEL_6128;
    cfg->memory_kb = 128;
    snprintf(cfg->rom_os,    sizeof(cfg->rom_os),    "%s", DEFAULT_ROM_OS);
    snprintf(cfg->rom_basic, sizeof(cfg->rom_basic), "%s", DEFAULT_ROM_BASIC);
    cfg->m4        = false;
    cfg->ulifac    = false;
    cfg->net4cpc   = false;
    cfg->scale     = 2;
    cfg->fullscreen= false;
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
        "\n"
        "[hardware]\n"
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
        } else if (!strcmp(section, "hardware")) {
            bool b;
            if (!strcmp(key, "m4")) {
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
        "basic=%s\n\n"
        "[hardware]\n"
        "m4=%s\n"
        "ulifac=%s\n"
        "net4cpc=%s\n\n"
        "[display]\n"
        "scale=%d\n"
        "fullscreen=%s\n",
        cfg->model == MODEL_464 ? "464" : "6128",
        cfg->memory_kb,
        cfg->rom_os,
        cfg->rom_basic,
        cfg->m4      ? "true" : "false",
        cfg->ulifac  ? "true" : "false",
        cfg->net4cpc ? "true" : "false",
        cfg->scale,
        cfg->fullscreen ? "true" : "false"
    );

    fclose(f);
    return 0;
}
