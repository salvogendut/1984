CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=

TEST_GATE_ARRAY = test-gate-array

.PHONY: all check clean

all: $(TEST_GATE_ARRAY)

$(TEST_GATE_ARRAY): test_gate_array.c ../src/gate_array.c ../src/gate_array.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -I../src \
		test_gate_array.c ../src/gate_array.c -o $@

check: $(TEST_GATE_ARRAY)
	./$(TEST_GATE_ARRAY)

clean:
	rm -f $(TEST_GATE_ARRAY)
