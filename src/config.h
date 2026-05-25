#pragma once
#include <stdbool.h>
#include "cpc.h"

#define CONFIG_PATH_MAX 512

typedef struct {
    /* [machine] */
    CpcModel model;
    int      memory_kb;     /* 64 or 128 */

    /* [roms] */
    char rom_os[CONFIG_PATH_MAX];
    char rom_basic[CONFIG_PATH_MAX];

    /* [storage] */
    char disk_a[CONFIG_PATH_MAX];
    char disk_b[CONFIG_PATH_MAX];

    /* [hardware] */
    bool m4;
    bool ulifac;
    bool net4cpc;

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
