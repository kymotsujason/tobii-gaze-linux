# gaze-cal/Makefile
CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -g
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -g
# Grows as tasks 11+ land src/client.c, src/display.c, src/calibrate.c,
# src/record.c and src/main.c. A fixed list breaks every build until then.
SRC      = $(wildcard src/*.c)
BUILD    = build

# No main() exists yet, so the CLI link target cannot be the default goal.
# Task 11 flips this back to $(BUILD)/gaze-cal.
.DEFAULT_GOAL := test

$(BUILD)/gaze-cal: $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm
$(BUILD):
	mkdir -p $(BUILD)
test: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/test_proto tests/test_proto.c src/proto.c && $(BUILD)/test_proto

# Proves the parser never reads past the caller's buffer and never relies on
# undefined behaviour. The exact-fit heap tests are meaningless without it.
test-asan: | $(BUILD)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/test_proto_asan tests/test_proto.c src/proto.c && \
	    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/test_proto_asan

# Plan 2's OBS plugin is C++ and includes proto.h directly. proto.o is built by
# the C compiler and linked by the C++ one on purpose: a header that parses but
# lacks extern "C" passes the compile and fails here, at the link.
test-cxx: | $(BUILD)
	$(CC) $(CFLAGS) -c src/proto.c -o $(BUILD)/proto_c.o
	$(CXX) $(CXXFLAGS) -c tests/test_proto_cxx.cpp -o $(BUILD)/test_proto_cxx.o
	$(CXX) -o $(BUILD)/test_proto_cxx $(BUILD)/test_proto_cxx.o $(BUILD)/proto_c.o
	$(BUILD)/test_proto_cxx

# Breaks proto.c one way at a time and requires the suite to notice. Proves the
# tests are load-bearing rather than back-filled.
mutate:
	bash tests/mutate.sh

check: test test-asan test-cxx mutate

clean:
	rm -rf $(BUILD)
.PHONY: test test-asan test-cxx mutate check clean
