# Terminator — custom terminal emulator and shell in C (Linux/X11)
#
# Targets:
#   make            build ./terminator
#   make test       build and run the automated test suite
#   make debug      build with debug symbols and no optimisation
#   make sanitize   build with AddressSanitizer + UBSan
#   make clean      remove build artefacts

# Compiler and flags are overridable, so the same project can use the TA's
# installed toolchain instead of relying on a machine-specific compiler path.
CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic
OPT     ?= -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# Xft (antialiased text) pulls in freetype headers.
# Ask pkg-config for the selected system's Xft include directories; no source
# or library path is tied to the developer's machine.
CPPFLAGS += $(shell pkg-config --cflags xft 2>/dev/null)
LDLIBS  += -lX11 -lXft -lXrender -lpthread

# Core modules are GUI-free so they can be linked into headless tests.
# Keep the shell core separate from X11 so tests do not need a display server.
CORE_SRC := \
	src/util.c \
	src/lexer.c \
	src/parser.c \
	src/expand.c \
	src/executor.c \
	src/jobs.c \
	src/history.c \
	src/completion.c \
	src/config.c \
	src/scripts.c \
	src/shell.c \
	src/multiwatch.c \
	src/ipc.c \
	src/tab.c

# Only the application binary adds startup, shared app state, and X11 GUI code.
APP_SRC := src/main.c src/app.c src/gui.c

CORE_OBJ := $(CORE_SRC:.c=.o)
APP_OBJ  := $(APP_SRC:.c=.o)
ALL_OBJ  := $(CORE_OBJ) $(APP_OBJ)

BIN := terminator
TEST_BIN := tests/run_tests

.PHONY: all clean test debug sanitize

all: $(BIN)

$(BIN): $(ALL_OBJ)
	$(CC) $(CFLAGS) -o $@ $(ALL_OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# Headless test binary: core modules plus app tab bookkeeping. gui_run() is
# stubbed by the harness, so no X11 library or display is needed.
$(TEST_BIN): $(CORE_OBJ) src/app.o tests/test_main.o
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJ) src/app.o tests/test_main.o -lpthread

tests/test_main.o: tests/test_main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test: $(TEST_BIN)
	@if ./$(TEST_BIN); then \
		exit 0; \
	else \
		rc=$$?; \
		if [ $$rc -eq 126 ] || [ $$rc -eq 127 ]; then \
			d="$${TMPDIR:-/tmp}"; \
			cp $(TEST_BIN) "$$d/terminator_run_tests"; \
			chmod +x "$$d/terminator_run_tests"; \
			"$$d/terminator_run_tests"; rc=$$?; \
			rm -f "$$d/terminator_run_tests"; \
		fi; \
		exit $$rc; \
	fi

debug: CFLAGS := $(CSTD) $(WARN) -O0 -g3
debug: clean $(BIN)

sanitize: CFLAGS := $(CSTD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: clean $(BIN) $(TEST_BIN)

clean:
	rm -f $(ALL_OBJ) $(BIN) $(TEST_BIN) tests/test_main.o
