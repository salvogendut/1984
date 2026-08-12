CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=

SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3)

TESTS = test-gate-array test-crtc test-z80 test-psg test-ppi test-io-decode test-mem test-disk test-gifcap test-tape-signal test-real-tape test-kbd test-cartridge test-asic test-model test-config test-snapshot

.PHONY: all check clean

all: $(TESTS)

test-gate-array: test_gate_array.c ../src/gate_array.c ../src/gate_array.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_gate_array.c ../src/gate_array.c -o $@

test-crtc: test_crtc.c ../src/crtc.c ../src/crtc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_crtc.c ../src/crtc.c -o $@

test-z80: test_z80.c ../src/z80.c ../src/z80.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_z80.c ../src/z80.c -o $@

test-psg: test_psg.c ../src/psg.c ../src/psg.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_psg.c ../src/psg.c -o $@

test-ppi: test_ppi.c ../src/ppi.c ../src/ppi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_ppi.c ../src/ppi.c -o $@

test-io-decode: test_io_decode.c ../src/io_decode.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_io_decode.c -o $@

test-mem: test_mem.c ../src/mem.c ../src/mem.h ../src/cartridge.c ../src/cartridge.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_mem.c ../src/mem.c ../src/cartridge.c -o $@

test-disk: test_disk.c ../src/disk.c ../src/disk.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_disk.c ../src/disk.c -o $@

test-gifcap: test_gifcap.c ../src/gifcap.c ../src/gifcap.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_gifcap.c ../src/gifcap.c -o $@

test-tape-signal: test_tape_signal.c ../src/tape.c ../src/tape.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_tape_signal.c ../src/tape.c -o $@

test-real-tape: test_real_tape.c ../src/real_tape.c ../src/real_tape.h ../src/tape.c ../src/tape.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_real_tape.c ../src/real_tape.c ../src/tape.c $(SDL3_LIBS) -o $@

test-kbd: test_kbd.c ../src/kbd.c ../src/kbd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_kbd.c ../src/kbd.c -o $@

test-cartridge: test_cartridge.c ../src/cartridge.c ../src/cartridge.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_cartridge.c ../src/cartridge.c -o $@

test-asic: test_asic.c ../src/asic.c ../src/asic.h ../src/mem.c ../src/cartridge.c ../src/gate_array.c ../src/psg.c ../src/crtc.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_asic.c ../src/asic.c ../src/mem.c ../src/cartridge.c \
		../src/gate_array.c ../src/psg.c ../src/crtc.c -lm -o $@

test-model: test_model.c ../src/cpc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_model.c -o $@

test-config: test_config.c ../src/config.c ../src/config.h ../src/real_tape.c ../src/tape.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_config.c ../src/config.c ../src/real_tape.c ../src/tape.c \
		$(SDL3_LIBS) -o $@

test-snapshot: test_snapshot.c ../src/snapshot.c ../src/remu.c ../src/symbols.c \
		../src/mem.c ../src/cartridge.c ../src/gate_array.c ../src/crtc.c \
		../src/ppi.c ../src/psg.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SDL3_CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_snapshot.c ../src/snapshot.c ../src/remu.c ../src/symbols.c \
		../src/mem.c ../src/cartridge.c ../src/gate_array.c ../src/crtc.c \
		../src/ppi.c ../src/psg.c -o $@

check: $(TESTS)
	./test-gate-array
	./test-crtc
	./test-z80
	./test-psg
	./test-ppi
	./test-io-decode
	./test-mem
	./test-disk
	./test-gifcap
	./test-tape-signal
	./test-real-tape
	./test-kbd
	./test-cartridge
	./test-asic
	./test-model
	./test-config
	./test-snapshot

clean:
	rm -f $(TESTS)
