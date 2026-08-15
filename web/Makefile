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
BUILD_VERSION ?= $(shell awk -F'[][]' '/^AC_INIT/ { print $$4; exit }' ../configure.ac)
BUILD_COMMIT  ?= $(shell git -C .. rev-parse --short HEAD 2>/dev/null || echo unknown)
CFLAGS    = -O2 -std=c11 -Wall -Wextra -I../src -Icompat -I. \
	-DPACKAGE_VERSION=\"$(BUILD_VERSION)\" -DPROG_GIT_COMMIT=\"$(BUILD_COMMIT)\"
DEPFLAGS  = -MMD -MP

SRCS = \
	../src/z80.c \
	../src/z80dis.c \
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
	../src/remu.c \
	../src/symbols.c \
	../src/snapshot.c \
	../src/paste.c \
	../src/cpc_breakpoint.c \
	../src/cpc.c

CORE_OBJS = $(patsubst ../src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS = $(CORE_OBJS) $(BUILD_DIR)/poc_main.o $(BUILD_DIR)/stubs.o $(BUILD_DIR)/m4_web.o
DEPS = $(OBJS:.o=.d)
WASM_ASSETS = $(DIST_DIR)/6128.js $(DIST_DIR)/6128.wasm
BRAND_ICON = ../icons/512x512/apps/io.github.salvogendut.Emulator1984.png
STYLE_ASSETS = styles.css theme-retro-crt.css \
	theme-sapporo.css theme-sapporo-dark.css theme-cpc464.css

all: $(DIST_DIR)/index.html $(addprefix $(DIST_DIR)/,$(STYLE_ASSETS)) $(DIST_DIR)/app.js \
	$(DIST_DIR)/gamepad.js $(DIST_DIR)/media-url.js $(DIST_DIR)/ml-monitor.js $(DIST_DIR)/dap.js \
	$(DIST_DIR)/m4-relay-protocol.js $(DIST_DIR)/m4-bridge.js \
	$(DIST_DIR)/brand-1984.png $(WASM_ASSETS)

test: test-gamepad test-media-url test-ml-monitor test-dap test-m4-bridge test-m4-wasm

test-gamepad:
	node test_gamepad.js

test-media-url:
	node test_media_url.js

test-ml-monitor:
	node test_ml_monitor.js

test-dap:
	node test_dap.js

test-m4-bridge:
	node test_m4_bridge.js

test-wasm: $(WASM_ASSETS)
	node test_dap_wasm.js

test-m4-wasm: $(WASM_ASSETS)
	node test_m4_wasm.js

$(WASM_ASSETS) &: $(OBJS) | $(DIST_DIR)
	$(EMCC) $(CFLAGS) $(OBJS) -o $(DIST_DIR)/6128.js \
	    -sMODULARIZE -sEXPORT_NAME=create6128 \
	    -sEXPORTED_FUNCTIONS='_poc_init,_poc_init_model,_poc_load_cartridge,_poc_reset,_poc_set_m4,_poc_m4_enabled,_poc_mount_m4_sd,_poc_eject_m4_sd,_poc_m4_sd_activity,_poc_m4_net_activity,_poc_set_memory_kb,_poc_memory_kb,_poc_load_snapshot,_poc_save_snapshot,_poc_step,_poc_pixels,_poc_key,_poc_joy,_poc_joy_matrix,_poc_set_mouse,_poc_mouse_move,_poc_mouse_button,_poc_disk_motor,_poc_disk_inserted,_poc_load_disk,_poc_eject_disk,_poc_tape_load,_poc_tape_eject,_poc_tape_play,_poc_tape_stop,_poc_tape_rewind,_poc_tape_next,_poc_tape_loaded,_poc_tape_motor,_poc_tape_playing,_poc_tape_paused,_poc_tape_ended,_poc_tape_counter,_poc_autorun,_poc_audio_reset,_poc_audio_avail,_poc_audio_read_pos,_poc_audio_buffer,_poc_audio_advance,_poc_width,_poc_height,_poc_build_version,_poc_build_commit,_poc_debug_pause,_poc_debug_continue,_poc_debug_step_in,_poc_debug_next,_poc_debug_step_back,_poc_debug_step_out,_poc_debug_is_paused,_poc_debug_stop_reason,_poc_debug_can_step_back,_poc_debug_reg,_poc_debug_breakpoint_set,_poc_debug_breakpoint_clear,_poc_debug_breakpoint_enabled,_poc_debug_breakpoint_addr,_poc_debug_breakpoint_count,_poc_debug_breakpoint_id_at,_poc_debug_breakpoint_source,_poc_set_snapshot_breakpoints,_poc_snapshot_breakpoints,_poc_debug_mem_read,_poc_debug_mem_write_byte,_poc_debug_disassemble,_poc_debug_watch_set,_poc_debug_watch_clear,_poc_debug_watch_serial,_poc_debug_watch_event_slot,_poc_debug_watch_event_addr,_poc_debug_watch_event_pc,_poc_debug_watch_event_old,_poc_debug_watch_event_new,_malloc,_free' \
	    -sEXPORTED_RUNTIME_METHODS='HEAPU8,HEAPU32,wasmMemory,getValue,setValue,ccall,cwrap,FS' \
	    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 \
	    --embed-file ../roms/OS_6128.ROM@roms/OS_6128.ROM \
	    --embed-file ../roms/BASIC_1.1.ROM@roms/BASIC_1.1.ROM \
	    --embed-file ../roms/AMSDOS.ROM@roms/AMSDOS.ROM \
	    --embed-file ../roms/M4ROM.ROM@roms/M4ROM.ROM \
	    --embed-file ../roms/system.cpr@roms/system.cpr

$(BUILD_DIR)/%.o: ../src/%.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/poc_main.o: poc_main.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) $(DEPFLAGS) -c poc_main.c -o $@

$(BUILD_DIR)/stubs.o: stubs.c | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) $(DEPFLAGS) -c stubs.c -o $@

$(BUILD_DIR)/m4_web.o: m4_web.c m4_web.h | $(BUILD_DIR)
	$(EMCC) $(CFLAGS) $(DEPFLAGS) -c m4_web.c -o $@

-include $(DEPS)

$(DIST_DIR)/index.html: index.html | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/gamepad.js: gamepad.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/media-url.js: media-url.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/ml-monitor.js: ml-monitor.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/dap.js: dap.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/m4-relay-protocol.js: m4-relay-protocol.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/m4-bridge.js: m4-bridge.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/%.css: %.css | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/app.js: app.js | $(DIST_DIR)
	cp $< $@

$(DIST_DIR)/brand-1984.png: $(BRAND_ICON) | $(DIST_DIR)
	cp $< $@

$(BUILD_DIR) $(DIST_DIR):
	mkdir -p $@

serve: all
	python3 -m http.server 8080 --directory $(DIST_DIR)

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
	rm -f *.o *.ppm poc.js poc.wasm 6128.js 6128.wasm

.PHONY: all clean serve test test-gamepad test-media-url test-ml-monitor test-dap test-m4-bridge test-wasm test-m4-wasm
