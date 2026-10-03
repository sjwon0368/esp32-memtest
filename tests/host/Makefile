# Host unit tests for esp32-memtest.
#
#   make run        - engine tests
#   make ui         - render the terminal screen and check its layout
#   make render     - replay real ANSI output through a terminal emulator
#   make asan       - engine tests under ASan/UBSan
#   make all        - everything

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion \
           -Wstrict-prototypes -Wmissing-prototypes -Wvla
# the UI test captures stdout through POSIX file descriptor calls
TESTDEFS := -D_DEFAULT_SOURCE
CORE    := ../../components/memtest_core
UI      := ../../components/memtest_ui
BUILD   := build
CORE_SRC := $(CORE)/mt_pattern.c $(CORE)/mt_pool.c $(CORE)/mt_test.c $(CORE)/mt_set.c

.PHONY: all run ui render render-keep asan clean

all: run ui render

run: $(BUILD)/test_core
	@$(BUILD)/test_core

ui: $(BUILD)/test_ui
	@$(BUILD)/test_ui

$(BUILD)/render_demo: render_demo.c $(UI)/mt_screen.c $(CORE_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TESTDEFS) -I$(CORE)/include -I$(UI)/include -o $@ $^

render: $(BUILD)/render_demo
	@$(BUILD)/render_demo

render-keep: $(BUILD)/render_demo
	@$(BUILD)/render_demo --print
	@echo "stream kept in $(BUILD)/screen.ansi"

$(BUILD)/test_core: test_core.c $(CORE_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE)/include -o $@ $^

$(BUILD)/test_ui: test_ui.c $(UI)/mt_screen.c $(CORE_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(TESTDEFS) -DMT_UI_FORCE_ANSI=1 -I$(CORE)/include -I$(UI)/include -o $@ $^

asan:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-I$(CORE)/include -o $(BUILD)/test_core_asan test_core.c $(CORE_SRC)
	@$(BUILD)/test_core_asan

clean:
	rm -rf $(BUILD)
