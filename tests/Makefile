CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=

TESTS = test-gate-array test-crtc test-z80 test-psg test-mem test-disk

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

check: $(TESTS)
	./test-gate-array
	./test-crtc
	./test-z80
	./test-psg
	./test-mem
	./test-disk

clean:
	rm -f $(TESTS)
