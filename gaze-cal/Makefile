# gaze-cal/Makefile
CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -g
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O2 -g
# Grows as tasks 12+ land src/display.c, src/calibrate.c and src/record.c.
# A fixed list breaks every build until then.
SRC      = $(wildcard src/*.c)
BUILD    = build
# proto.c uses hypot and atan2 for the display-area conversion, so every target
# that links it needs libm, not only the CLI.
LDLIBS   = -lm
# Only the CLI links X. stimulus.c is the one translation unit that includes
# Xlib, which is why the test targets below can build the calibration sequence
# without a display anywhere.
XLIBS    = -lX11 -lXrandr

.DEFAULT_GOAL := $(BUILD)/gaze-cal

$(BUILD)/gaze-cal: $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS) $(XLIBS)
$(BUILD):
	mkdir -p $(BUILD)
test: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/test_proto tests/test_proto.c src/proto.c $(LDLIBS) && $(BUILD)/test_proto
	$(CC) $(CFLAGS) -o $(BUILD)/test_client tests/test_client.c src/client.c src/proto.c $(LDLIBS) && $(BUILD)/test_client
	$(CC) $(CFLAGS) -o $(BUILD)/test_display tests/test_display.c src/display.c src/client.c src/proto.c $(LDLIBS) && $(BUILD)/test_display
	$(CC) $(CFLAGS) -o $(BUILD)/test_calibrate tests/test_calibrate.c src/calibrate.c src/display.c src/client.c src/proto.c $(LDLIBS) && $(BUILD)/test_calibrate

# Proves the parser never reads past the caller's buffer and never relies on
# undefined behaviour. The exact-fit heap tests are meaningless without it.
test-asan: | $(BUILD)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/test_proto_asan tests/test_proto.c src/proto.c $(LDLIBS) && \
	    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/test_proto_asan
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/test_client_asan tests/test_client.c src/client.c src/proto.c $(LDLIBS) && \
	    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/test_client_asan
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/test_display_asan tests/test_display.c src/display.c src/client.c src/proto.c $(LDLIBS) && \
	    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/test_display_asan
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD)/test_calibrate_asan tests/test_calibrate.c src/calibrate.c src/display.c src/client.c src/proto.c $(LDLIBS) && \
	    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/test_calibrate_asan

# Plan 2's OBS plugin is C++ and includes these headers directly. The objects
# are built by the C compiler and linked by the C++ one on purpose: a header
# that parses but lacks extern "C" passes the compile and fails here, at the
# link.
test-cxx: | $(BUILD)
	$(CC) $(CFLAGS) -c src/proto.c -o $(BUILD)/proto_c.o
	$(CC) $(CFLAGS) -c src/client.c -o $(BUILD)/client_c.o
	$(CXX) $(CXXFLAGS) -c tests/test_proto_cxx.cpp -o $(BUILD)/test_proto_cxx.o
	$(CXX) -o $(BUILD)/test_proto_cxx $(BUILD)/test_proto_cxx.o $(BUILD)/proto_c.o $(LDLIBS)
	$(BUILD)/test_proto_cxx
	$(CXX) $(CXXFLAGS) -c tests/test_client_cxx.cpp -o $(BUILD)/test_client_cxx.o
	$(CXX) -o $(BUILD)/test_client_cxx $(BUILD)/test_client_cxx.o $(BUILD)/client_c.o $(BUILD)/proto_c.o $(LDLIBS)
	$(BUILD)/test_client_cxx

# Breaks proto.c and client.c one way at a time and requires the suite to
# notice. Proves the tests are load-bearing rather than back-filled.
mutate:
	bash tests/mutate.sh

# Needs a running tobiifreed and a connected tracker, so it is not part of
# `check`. Exits 77 when the daemon is not reachable.
test-live: | $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/test_client_live tests/test_client_live.c src/client.c src/proto.c $(LDLIBS)
	$(BUILD)/test_client_live

check: test test-asan test-cxx mutate

clean:
	rm -rf $(BUILD)
.PHONY: test test-asan test-cxx test-live mutate check clean
