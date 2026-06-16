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
	src/kbd_pty.c \
	src/screen_text.c \
	src/ppi.c \
	src/psg.c \
	src/rtc.c \
	src/ide.c \
	src/fat.c \
	src/ch376.c \
	src/gifcap.c \
	src/webmcap.c \
	src/host_mount.c \
	src/leds.c \
	src/m4.c \
	src/symbnet.c \
	src/symbos_trace.c \
	src/mouse.c \
	src/n4c_stack.c \
	src/snapshot.c \
	src/tap.c \
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
	src/gifcap.h \
	src/webmcap.h \
	src/host_mount.h \
	src/ide.h \
	src/joy.h \
	src/kbd.h \
	src/leds.h \
	src/m4.h \
	src/mem.h \
	src/monitor.h \
	src/mouse.h \
	src/net4cpc.h \
	src/n4c_stack.h \
	src/overlay.h \
	src/tap.h \
	src/paste.h \
	src/ppi.h \
	src/psg.h \
	src/rtc.h \
	src/shutter_wav.h \
	src/snapshot.h \
	src/symbnet.h \
	src/symbos_trace.h \
	src/tape.h \
	src/types.h \
	src/z80.h \
	src/z80dis.h

1984_CFLAGS  = $(AM_CFLAGS) $(SDL3_CFLAGS) -Wall -Wextra \
               -DROM_INSTALL_DIR=\"$(pkgdatadir)/roms\" \
               -DPROG_GIT_COMMIT=\"$(PROG_GIT_COMMIT)\"
# On Windows the resource object compiled below carries the .ico into
# the .exe; SDL3's -mwindows (linked via $(SDL3_LIBS)) suppresses the
# console window. _DEPENDENCIES is needed so make builds the .o before
# the link step — items only in _LDADD are linked but not built.
1984_LDADD        = $(SDL3_LIBS) -lm $(WIN_LIBS) $(WIN_RC_OBJ)
1984_DEPENDENCIES = $(WIN_RC_OBJ)

icons/1984-rc.o: $(srcdir)/icons/1984.rc $(srcdir)/icons/1984.ico
	@mkdir -p icons
	$(WINDRES) -I$(srcdir)/icons -i $< -O coff -o $@

CLEANFILES = icons/1984-rc.o

# Bundled ROM images installed to $(datadir)/1984/roms/
# (CPC firmware are freely redistributable for emulator use; M4ROM and
# Amstrad diagnostics ROM are open source.)
romsdir       = $(pkgdatadir)/roms
dist_roms_DATA = roms/AMSDOS.ROM \
                 roms/AMSDOS_664.ROM \
                 roms/BASIC_1.0.ROM \
                 roms/BASIC_1.1.ROM \
                 roms/BASIC_664.ROM \
                 roms/M4ROM.ROM \
                 roms/OS_464.ROM \
                 roms/OS_664.ROM \
                 roms/OS_6128.ROM \
                 roms/AmstradDiagLower.rom

# Desktop integration: .desktop launcher, AppStream metainfo, hicolor icons.
desktopdir         = $(datadir)/applications
dist_desktop_DATA  = io.github.salvogendut.Emulator1984.desktop

metainfodir        = $(datadir)/metainfo
dist_metainfo_DATA = io.github.salvogendut.Emulator1984.metainfo.xml

icon16dir          = $(datadir)/icons/hicolor/16x16/apps
dist_icon16_DATA   = icons/16x16/apps/io.github.salvogendut.Emulator1984.png
icon32dir          = $(datadir)/icons/hicolor/32x32/apps
dist_icon32_DATA   = icons/32x32/apps/io.github.salvogendut.Emulator1984.png
icon48dir          = $(datadir)/icons/hicolor/48x48/apps
dist_icon48_DATA   = icons/48x48/apps/io.github.salvogendut.Emulator1984.png
icon64dir          = $(datadir)/icons/hicolor/64x64/apps
dist_icon64_DATA   = icons/64x64/apps/io.github.salvogendut.Emulator1984.png
icon128dir         = $(datadir)/icons/hicolor/128x128/apps
dist_icon128_DATA  = icons/128x128/apps/io.github.salvogendut.Emulator1984.png
icon256dir         = $(datadir)/icons/hicolor/256x256/apps
dist_icon256_DATA  = icons/256x256/apps/io.github.salvogendut.Emulator1984.png
icon512dir         = $(datadir)/icons/hicolor/512x512/apps
dist_icon512_DATA  = icons/512x512/apps/io.github.salvogendut.Emulator1984.png

# Files shipped in the dist tarball but not built (license, hand-written
# Makefile alternative, project docs).
EXTRA_DIST = Makefile LICENSE Development.md icons/1984.rc icons/1984.ico
