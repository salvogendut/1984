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
	src/z80.c

1984_CFLAGS  = $(AM_CFLAGS) $(SDL3_CFLAGS) -Wall -Wextra
1984_LDADD   = $(SDL3_LIBS) -lm

# Keep the hand-written Makefile usable alongside autotools
EXTRA_DIST = Makefile
