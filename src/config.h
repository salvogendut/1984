#pragma once
#include <stdbool.h>
#include "cpc.h"

/* Re-declared here because including printer.h pulls in <limits.h>
 * and we want config.h to stay narrow. PrintSink: see src/printer.h. */
typedef enum { PRINTER_SINK_PDF = 0, PRINTER_SINK_REAL_PRINTER } ConfigPrintSink;

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
    /* Per-slot board membership, comma-separated. Set via the overlay
     * ROM menu's `Ins` key, validated against the whitelist
     * {m4, albireo, cyboard}. Sourced from `[board:NAME]` sections in
     * 1984.conf. When a board's matching hardware bool flips on (m4,
     * albireo, or symbiface_ide for cyboard), every slot tagged with
     * that board's name gets its template path loaded into rom_ext[].
     * Empty string = the slot is user-pinned, not board-managed. */
    char rom_ext_boards[ROM_EXT_COUNT][64];
    /* Per-board conf templates. Each `[board:NAME]` section in
     * 1984.conf holds the slot_N=PATH and image=PATH entries the
     * board needs to function. When the matching hardware toggle
     * flips on, the overlay copies these into the live cfg
     * (rom_ext[] + ide_image/albireo_image/m4_image) so the user
     * doesn't have to re-pick paths on every enable cycle.
     * Cleared per-field by pressing Del on the relevant row. */
    char board_m4_slot[ROM_EXT_COUNT][CONFIG_PATH_MAX];
    char board_albireo_slot[ROM_EXT_COUNT][CONFIG_PATH_MAX];
    char board_cyboard_slot[ROM_EXT_COUNT][CONFIG_PATH_MAX];
    char board_m4_image[CONFIG_PATH_MAX];        /* SD-card image */
    char board_albireo_image[CONFIG_PATH_MAX];   /* USB drive image */
    char board_cyboard_image[CONFIG_PATH_MAX];   /* IDE (SymbIface) image */

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
    bool usifac;                       /* USIfAC II serial interface (port &FBD0/D1) */
    char usifac_backend[16];           /* "pty" or "tcp" */
    int  usifac_tcp_port;              /* TCP listen port when backend=tcp (default 4001) */
    /* Optional stable host-side symlink to the live /dev/pts/N slave. When
     * non-empty (PTY backend only), open_pty() unlink()s any stale link and
     * symlink()s the slave path to it so external tools (minicom, pty_modem.py,
     * custom scripts) don't have to chase the randomised pts number each
     * launch. Empty disables the alias. POSIX only; ignored on Windows. */
    char usifac_pty_link[CONFIG_PATH_MAX];
    bool perryfi;                      /* PerryFi software AT-modem extension
                                          that turns the USIfAC port into a
                                          Hayes-style Wi-Fi modem bridging to
                                          host TCP via ATDT host:port. Gated on
                                          `usifac` — no point without a serial
                                          port to plug into. */
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
    /* When the dual-CH376 card is enabled, populate the second chip at
     * 0xFE40/41 so SymbOS can enumerate a USB HID mouse on chip A and
     * still serve disk I/O from chip B. When false, only chip A is
     * wired — matches a single-chip Albireo card and avoids the
     * polling overhead SymbOS's HID driver adds. */
    bool albireo_mouse;
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
    MonoMode monochrome;       /* off / green / amber / white phosphor tint */

    /* [printer] — host-side Centronics capture (port 0xEFxx).
     * pdf_printer=true  + pdf_printer_dir non-empty: write PDFs there.
     * sink=real         spools the PDF to the host's default CUPS
     *                   printer via `lp` (Linux/macOS).
     * Disabling pdf_printer makes the emulated port a no-op host-side
     * (the guest still sees "not busy" via PPI port B). */
    bool            pdf_printer;
    char            pdf_printer_dir[CONFIG_PATH_MAX];
    ConfigPrintSink print_sink;

    /* Last directory used by the F9 file pickers. Updated whenever the
     * user picks a file or folder; passed back as the default location
     * on the next dialog so each launch starts where the previous one
     * left off (issue #107). */
    char last_dir[CONFIG_PATH_MAX];

    /* [audio] */
    int  audio_volume;        /* 0..100, perceptual curve; default 80 */
    int  audio_stereo_sep;    /* 0..255: 0 = mono, 255 = full Caprice32
                               * ABC panning (A left, B centre, C right);
                               * default 0 (mono) */

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

/* Same as config_load but reads from path_override (if non-NULL and non-empty)
 * instead of ~/.config/1984/1984.conf. config_save() still targets the user's
 * default path, so an override is read-only. */
int config_load_from(Config *cfg, const char *path_override);

/* Fill cfg with compiled-in defaults. */
void config_defaults(Config *cfg);

/* Write current cfg back to ~/.config/1984/1984.conf. */
int config_save(const Config *cfg);

/* Switch model and apply matching RAM size and ROM path defaults. */
void config_set_model(Config *cfg, CpcModel model);

/* Enable or disable the DDI-1 on a CPC 464 (sets/clears rom_amsdos). */
void config_apply_dd1(Config *cfg, bool enabled);

/* Whitelist of board names usable as ROM-slot tags. */
#define CONFIG_BOARDS  (const char *[]){"m4", "albireo", "cyboard"}
#define CONFIG_BOARDS_COUNT 3

/* Returns the per-board template-paths table for `board`, or NULL if
 * board isn't a known name. Pointer is into the supplied Config. */
char (*config_board_slots(Config *cfg, const char *board))[CONFIG_PATH_MAX];

/* Returns the per-board cached-image-path buffer for `board`
 * (sized CONFIG_PATH_MAX), or NULL if board isn't a known name. */
char *config_board_image(Config *cfg, const char *board);

/* Normalise + validate a comma-separated board list (e.g. user input
 * from the overlay `Ins` editor). Drops unknown tokens with a stderr
 * warning, trims whitespace, lowercases, deduplicates. Writes the
 * canonical form into `out` (size `out_sz`). */
void config_normalize_boards(const char *in, char *out, size_t out_sz);

/* True if `board` appears in the comma-separated list `csv`. */
bool config_boards_contains(const char *csv, const char *board);

/* Reapply board membership to rom_ext[]: for every slot that names a
 * board whose hardware bool is on, copy the board's template path into
 * rom_ext[]; for every slot whose only board(s) are now disabled,
 * clear rom_ext[]. Returns the number of slots whose path changed. */
int config_apply_boards(Config *cfg);

/* Restore individual ROM paths to the compiled-in defaults for the model. */
void config_default_os(CpcModel model, char *out, size_t sz);
void config_default_basic(CpcModel model, char *out, size_t sz);
void config_default_amsdos(CpcModel model, char *out, size_t sz);
void config_default_m4rom(char *out, size_t sz);
void config_default_diag(char *out, size_t sz);
