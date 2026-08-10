# WASM POC build for the 1984 core (issue #258).
#
# Compiles a reduced source set (the emulation core + chip files) plus web
# stubs for host-only modules, with Emscripten. Run inside the my-distrobox
# container where emcc and the SDL3 headers live.
#
#   make clean && make
#   python3 -m http.server 8080 --directory dist
#
# Produces a self-contained publish directory under dist/. Compiler
# intermediates stay under build/.

EMCC      = emcc
BUILD_DIR = build
DIST_DIR  = dist
CFLAGS    = -O2 -std=c11 -Wall -Wextra -I../src -Icompat -I.

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

CORE_OBJS = $(patsubst ../src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS = $(CORE_OBJS) $(BUILD_DIR)/poc_main.o $(BUILD_DIR)/stubs.o
WASM_ASSETS = $(DIST_DIR)/6128.js $(DIST_DIR)/6128.wasm

all: $(DIST_DIR)/index.html $(DIST_DIR)/styles.css $(DIST_DIR)/app.js \
	$(DIST_DIR)/gamepad.js $(WASM_ASSETS)

test-gamepad:
	node test_gamepad.js

$(WASM_ASSETS) &: $(OBJS) | $(DIST_DIR)
	$(EMCC) $(CFLAGS) $(OBJS) -o $(DIST_DIR)/6128.js \
	    -sMODULARIZE -sEXPORT_NAME=create6128 \
	    -sEXPORTED_FUNCTIONS='_poc_init,_poc_init_model,_poc_load_cartridge,_poc_reset,_poc_step,_poc_pixels,_poc_key,_poc_joy,_poc_joy_matrix,_poc_set_mouse,_poc_mouse_move,_poc_mouse_button,_poc_disk_motor,_poc_load_disk,_poc_eject_disk,_poc_audio_reset,_poc_audio_avail,_poc_audio_read_pos,_poc_audio_buffer,_poc_audio_advance,_poc_width,_poc_height,_malloc,_free' \
	    -sEXPORTED_RUNTIME_METHODS='HEAPU8,HEAPU32,wasmMemory,getValue,setValue,ccall,cwrap,FS' \
	    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
	    --embed-file ../roms/OS_6128.ROM@roms/OS_6128.ROM \
	    --embed-file ../roms/BASIC_1.1.ROM@roms/BASIC_1.1.ROM \
	    --embed-file ../roms/AMSDOS.ROM@roms/AMSDOS.ROM \
	    --embed-file ../roms/system.cpr@roms/system.cpr

$(BUILD_DIR)/%.o: ../src/%.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/poc_main.o: poc_main.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) -c poc_main.c -o $@

$(BUILD_DIR)/stubs.o: stubs.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) -c stubs.c -o $@

$(DIST_DIR)/index.html: index.html | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/gamepad.js: gamepad.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/styles.css: styles.css | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/app.js: app.js | $(DIST_DIR)
	cp $< $@

$(BUILD_DIR) $(DIST_DIR):
	mkdir -p $@

serve: all
	python3 -m http.server 8080 --directory $(DIST_DIR)

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
	rm -f *.o *.ppm poc.js poc.wasm 6128.js 6128.wasm

.PHONY: all clean serve test-gamepad
