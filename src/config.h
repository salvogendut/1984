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
    char tape[CONFIG_PATH_MAX];   /* .cdt tape image */
    bool external_tape;           /* CPC 6128: emulate an external cassette deck */

    /* [hardware] */
    bool mx4;        /* MX4 expansion bus connected (gates all extension I/O) */
    bool rom_board;  /* ROM Board fitted — when false, ignore rom_ext[] at boot */
    bool dd1;      /* CPC 464 only: DDI-1 floppy interface + AMSDOS ROM */
    bool m4;
    char m4_path[CONFIG_PATH_MAX];   /* host directory for M4 file API (cat/load/save) */
    char m4_image[CONFIG_PATH_MAX];  /* optional raw FAT32 image for C_SDREAD/C_SDWRITE */
    bool ulifac;
    bool net4cpc;
    bool net4cpc_tap;  /* Use TAP backend: 1984 auto-creates the tap device,
                        * runs a built-in DHCP server, adds the interface to
                        * the firewalld trusted zone. When false, Net4CPC
                        * falls back to the legacy POSIX host-socket path. */
    /* DHCP server / TAP network parameters. All four are dotted-quad IPv4
     * strings. The host IP is the address we assign to the tap interface;
     * lease_start/end define the range the in-process DHCP server hands
     * out (a single CPC will always lease the first free IP). Netmask is
     * also dotted-quad so /etc/sysctl-style "/24" doesn't need parsing. */
    char net4cpc_tap_host_ip   [24];   /* default "10.0.0.1"     */
    char net4cpc_tap_netmask   [24];   /* default "255.255.255.0" */
    char net4cpc_tap_lease_start[24];  /* default "10.0.0.100"   */
    char net4cpc_tap_lease_end [24];   /* default "10.0.0.150"   */
    bool rtc;
    bool symbiface_ide;
    char ide_image[CONFIG_PATH_MAX];
    bool symbiface_mouse;
    bool symbnet;     /* 1984 emulator synthetic SymbOS network port (0xFD30/31) */
    bool albireo;
    char albireo_image[CONFIG_PATH_MAX];
    /* If true, the CH376 emulation refuses raw-sector DISK_READ commands so
     * SymbOS falls back to the chip's built-in FS via FILE_OPEN/BYTE_READ.
     * Works around a current rendering bug where SymbOS-FAT-driver-via-raw-
     * DISK_READ corrupts on-screen text (icon labels, menus). The fallback
     * path renders correctly but apps fail to load with "disc error". */
    bool albireo_disable_disk_read;

    /* [display] */
    int  scale;             /* 1, 2, or 3 */
    bool fullscreen;
    bool fullscreen_smoothing; /* linear (smooth) vs nearest (sharp) texture scale */

    /* [advanced] */
    bool tinker;             /* enable Advanced overlay tab with low-level toggles */
    bool debug;              /* enable debug machinery (env-var traces, panic
                              * dumps, text capture). Off by default; when off,
                              * setting ONE_K_TRACE_* / ONE_K_CAP_TEXT / etc.
                              * has no effect at all. */
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
