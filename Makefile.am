bin_PROGRAMS = 1984

1984_SOURCES = \
	src/config.c \
	src/cpc.c \
	src/monitor.c \
	src/net4cpc.c \
	src/z80dis.c \
	src/crtc.c \
	src/disk.c \
	src/display.c \
	src/fdc.c \
	src/gate_array.c \
	src/joy.c \
	src/kbd.c \
	src/main.c \
	src/mem.c \
	src/overlay.c \
	src/paste.c \
	src/ppi.c \
	src/psg.c \
	src/rtc.c \
	src/ide.c \
	src/m4.c \
	src/mouse.c \
	src/z80.c

1984_CFLAGS  = $(AM_CFLAGS) $(SDL3_CFLAGS) -Wall -Wextra \
               -DROM_INSTALL_DIR=\"$(pkgdatadir)/roms\"
1984_LDADD   = $(SDL3_LIBS) -lm

# Bundled ROM images installed to $(datadir)/1984/roms/
# (CPC firmware are freely redistributable for emulator use; M4ROM and
# Amstrad diagnostics ROM are open source.)
romsdir       = $(pkgdatadir)/roms
dist_roms_DATA = roms/AMSDOS.ROM \
                 roms/BASIC_1.0.ROM \
                 roms/BASIC_1.1.ROM \
                 roms/M4ROM.ROM \
                 roms/OS_464.ROM \
                 roms/OS_6128.ROM \
                 roms/AmstradDiagLower.rom

# Keep the hand-written Makefile usable alongside autotools
EXTRA_DIST = Makefile
