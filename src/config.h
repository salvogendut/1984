#pragma once
#include <stdbool.h>
#include "cpc.h"

#define CONFIG_PATH_MAX 512

typedef struct {
    /* [machine] */
    CpcModel model;
    int      memory_kb;     /* 64, 128, 256, 512, or 576 */

    /* [roms] */
    char rom_os[CONFIG_PATH_MAX];
    char rom_basic[CONFIG_PATH_MAX];
    char rom_amsdos[CONFIG_PATH_MAX];

    /* [expansion_roms] — slot_0 … slot_31 */
    char rom_ext[ROM_EXT_COUNT][CONFIG_PATH_MAX];

    /* [storage] */
    char disk_a[CONFIG_PATH_MAX];
    char disk_b[CONFIG_PATH_MAX];

    /* [hardware] */
    bool dd1;      /* CPC 464 only: DDI-1 floppy interface + AMSDOS ROM */
    bool m4;
    char m4_path[CONFIG_PATH_MAX];   /* host directory for M4 file API (cat/load/save) */
    char m4_image[CONFIG_PATH_MAX];  /* optional raw FAT32 image for C_SDREAD/C_SDWRITE */
    bool ulifac;
    bool net4cpc;
    bool rtc;
    bool symbiface_ide;
    char ide_image[CONFIG_PATH_MAX];
    bool symbiface_mouse;

    /* [display] */
    int  scale;             /* 1, 2, or 3 */
    bool fullscreen;
} Config;

/* Load ~/.config/1984/1984.conf into cfg. Missing file = silent defaults.
 * Returns 0 on success, -1 if a value is invalid (error printed to stderr). */
int config_load(Config *cfg);

/* Fill cfg with compiled-in defaults. */
void config_defaults(Config *cfg);

/* Write current cfg back to ~/.config/1984/1984.conf. */
int config_save(const Config *cfg);

/* Switch model and apply matching RAM size and ROM path defaults. */
void config_set_model(Config *cfg, CpcModel model);

/* Enable or disable the DDI-1 on a CPC 464 (sets/clears rom_amsdos). */
void config_apply_dd1(Config *cfg, bool enabled);

/* Restore individual ROM paths to the compiled-in defaults for the model. */
void config_default_os(CpcModel model, char *out, size_t sz);
void config_default_basic(CpcModel model, char *out, size_t sz);
void config_default_amsdos(char *out, size_t sz);
void config_default_m4rom(char *out, size_t sz);
void config_default_diag(char *out, size_t sz);
