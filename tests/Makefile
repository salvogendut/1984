CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=

SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3)

TESTS = test-gate-array test-crtc test-z80 test-psg test-mem test-disk test-gifcap test-tape-signal test-real-tape test-kbd

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

test-mem: test_mem.c ../src/mem.c ../src/mem.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_mem.c ../src/mem.c -o $@

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

check: $(TESTS)
	./test-gate-array
	./test-crtc
	./test-z80
	./test-psg
	./test-mem
	./test-disk
	./test-gifcap
	./test-tape-signal
	./test-real-tape
	./test-kbd

clean:
	rm -f $(TESTS)
