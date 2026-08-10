# WASM POC build for the 1984 core (issue #258).
#
# Compiles a reduced source set (the emulation core + chip files) plus web
# stubs for host-only modules, with Emscripten. Run inside the my-distrobox
# container where emcc and the SDL3 headers live.
#
#   make clean && make
#
# Produces poc.js + poc.wasm (MODULARIZE; factory = createPoc) with the ROMs
# embedded in the virtual filesystem at /roms.

EMCC      = emcc
# SDL3 headers are vendored under web/SDL3 (copied from the container's
# /usr/include/SDL3) so emcc does not pick up the host glibc headers.
CFLAGS    = -O2 -std=c11 -Wall -Wextra -I../src -I.

SRCS = \
	../src/z80.c \
	../src/crtc.c \
	../src/gate_array.c \
	../src/psg.c \
	../src/ppi.c \
	../src/mem.c \
	../src/disk.c \
	../src/fdc.c \
	../src/tape.c \
	../src/asic.c \
	../src/kbd.c \
	../src/ide.c \
	../src/ch376.c \
	../src/symbnet.c \
	../src/mouse.c \
	../src/amx.c \
	../src/rtc.c \
	../src/m4.c \
	../src/fat.c \
	../src/config.c \
	../src/cartridge.c \
	../src/snapshot.c \
	../src/cpc.c

OBJS = $(SRCS:../src/%.c=%.o) poc_main.o stubs.o

all: poc.js

poc.js: $(OBJS)
	$(EMCC) $(CFLAGS) $(OBJS) -o poc.js \
	    -sMODULARIZE -sEXPORT_NAME=createPoc \
	    -sEXPORTED_FUNCTIONS='_poc_init,_poc_step,_poc_pixels,_poc_key,_poc_joy,_poc_load_disk,_poc_audio_reset,_poc_audio_avail,_poc_audio_read_pos,_poc_audio_buffer,_poc_audio_advance,_poc_width,_poc_height,_malloc,_free' \
	    -sEXPORTED_RUNTIME_METHODS='HEAPU8,HEAPU32,wasmMemory,getValue,setValue,ccall,cwrap,FS' \
	    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
	    --embed-file ../roms/OS_6128.ROM@roms/OS_6128.ROM \
	    --embed-file ../roms/BASIC_1.1.ROM@roms/BASIC_1.1.ROM \
	    --embed-file ../roms/AMSDOS.ROM@roms/AMSDOS.ROM

%.o: ../src/%.c
	$(EMCC) $(CFLAGS) -c $< -o $@

poc_main.o: poc_main.c
	$(EMCC) $(CFLAGS) -c poc_main.c -o $@

stubs.o: stubs.c
	$(EMCC) $(CFLAGS) -c stubs.c -o $@

clean:
	rm -f *.o poc.js poc.wasm
