# =============================================================================
# Toolchain selection
#
# CC defaults to gcc but can be overridden on the command line:
#   make               -> builds with gcc (dynamic CLI/test binary, .a, .so)
#   make CC=musl-gcc   -> builds with musl-gcc instead
#
# STATIC controls whether the CLI binary and test binary are statically
# linked, independent of which CC is used:
#   make STATIC=1              -> static binary via gcc  (requires glibc-static)
#   make CC=musl-gcc STATIC=1  -> static binary via musl-gcc (musl is
#                                 static-link-friendly out of the box; no
#                                 extra glibc-static package needed)
#
# The previous version of this Makefile listed -static in CFLAGS, which is
# silently a no-op for linking: CFLAGS only reaches the per-file compile
# rule ($(OBJ_DIR)/%.o), never the final link command for TARGET_CLI, which
# invoked $(CC) $(LDFLAGS) with an always-empty LDFLAGS. -Wl,-static and
# -static are link-step flags; putting them in CFLAGS compiles fine (it's
# a harmless unused flag on a compile-only invocation) but never actually
# statically links anything, regardless of which CC you point at it.
# =============================================================================

CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O3
LDFLAGS =

# STATIC=1 appends -static to LDFLAGS so it reaches every link rule that
# should honor it. It is intentionally NOT added to CFLAGS (compile-only
# invocations don't need it) and intentionally NOT added to the shared
# library's link rule below (a "static shared library" is a contradiction;
# -static there would either be ignored or break -shared depending on
# toolchain, so that rule always links dynamically regardless of STATIC).
STATIC ?= 0
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
BINDIR ?= $(PREFIX)/bin

SRC_DIR = src
OBJ_DIR = build
BIN_DIR = bin
LIB_OUT_DIR = lib

LIB_SRC = $(SRC_DIR)/cnotify.c
APP_SRC = $(SRC_DIR)/main.c
TEST_SRC = $(SRC_DIR)/test.c

# Objects for static lib and executable
LIB_OBJ = $(OBJ_DIR)/cnotify.o
APP_OBJ = $(OBJ_DIR)/main.o

# Objects for shared lib (compiled with -fPIC)
LIB_OBJ_SHARED = $(OBJ_DIR)/cnotify_shared.o

TARGET_CLI = $(BIN_DIR)/cnotify
TARGET_TEST = $(BIN_DIR)/test_cnotify
TARGET_STATIC = $(LIB_OUT_DIR)/libcnotify.a
TARGET_SHARED = $(LIB_OUT_DIR)/libcnotify.so

all: $(TARGET_CLI) $(TARGET_TEST) $(TARGET_STATIC) $(TARGET_SHARED)

# CLI Tool
#
# CFLAGS is passed here (not just LDFLAGS) because CFLAGS also carries
# -Iinclude, which some linker frontends want on the final invocation too
# when object files reference relative include paths for LTO/debug info.
# LDFLAGS is listed last so -static from STATIC=1 wins over any earlier
# implicit linker defaults.
$(TARGET_CLI): $(LIB_OBJ) $(APP_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Tests
$(TARGET_TEST): $(LIB_OBJ) $(TEST_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Static Library (a .a archive — unrelated to the STATIC=1 linking option
# above; this target is always produced, since an archive has no
# static/dynamic distinction of its own).
$(TARGET_STATIC): $(LIB_OBJ) | $(LIB_OUT_DIR)
	ar rcs $@ $^

# Shared Library
#
# Deliberately does not append $(LDFLAGS): if STATIC=1 was set, mixing
# -static into a -shared link is either rejected outright or produces a
# broken .so depending on toolchain, so this rule always ignores STATIC
# and links a normal dynamic shared object.
$(TARGET_SHARED): $(LIB_OBJ_SHARED) | $(LIB_OUT_DIR)
	$(CC) -shared -o $@ $^

# Standard compilation
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Shared compilation (PIC)
$(OBJ_DIR)/%_shared.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(LIB_OUT_DIR):
	mkdir -p $(LIB_OUT_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(LIB_OUT_DIR)

test: $(TARGET_CLI) $(TARGET_TEST)
	./$(TARGET_TEST)

# Convenience targets:
#   make static       -> gcc,      CLI + test statically linked
#   make musl-static   -> musl-gcc, CLI + test statically linked
# Both are equivalent to setting CC/STATIC by hand; provided so the common
# case doesn't require remembering the variable names.
.PHONY: static musl-static
static:
	$(MAKE) CC=gcc STATIC=1 $(TARGET_CLI) $(TARGET_TEST)

musl-static:
	$(MAKE) CC=musl-gcc STATIC=1 $(TARGET_CLI) $(TARGET_TEST)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET_CLI) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(TARGET_STATIC) $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET_SHARED) $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 include/cnotify.h $(DESTDIR)$(INCLUDEDIR)

.PHONY: all clean test install
