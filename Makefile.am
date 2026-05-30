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
	src/fat.c \
	src/ch376.c \
	src/m4.c \
	src/symbnet.c \
	src/mouse.c \
	src/tape.c \
	src/z80.c

noinst_HEADERS = \
	src/ch376.h \
	src/config.h \
	src/cpc.h \
	src/crtc.h \
	src/disk.h \
	src/display.h \
	src/fat.h \
	src/fdc.h \
	src/gate_array.h \
	src/ide.h \
	src/joy.h \
	src/kbd.h \
	src/m4.h \
	src/mem.h \
	src/monitor.h \
	src/mouse.h \
	src/net4cpc.h \
	src/overlay.h \
	src/paste.h \
	src/ppi.h \
	src/psg.h \
	src/rtc.h \
	src/shutter_wav.h \
	src/symbnet.h \
	src/tape.h \
	src/types.h \
	src/z80.h \
	src/z80dis.h

1984_CFLAGS  = $(AM_CFLAGS) $(SDL3_CFLAGS) -Wall -Wextra \
               -DROM_INSTALL_DIR=\"$(pkgdatadir)/roms\"
# $(WIN_SUBSYSTEM) (-mconsole) must come LAST so it wins over the -mwindows
# that $(SDL3_LIBS) drags in.
1984_LDADD   = $(SDL3_LIBS) -lm $(WIN_LIBS) $(WIN_SUBSYSTEM)

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

# Files shipped in the dist tarball but not built (license, hand-written
# Makefile alternative, project docs).
EXTRA_DIST = Makefile LICENSE Development.md
