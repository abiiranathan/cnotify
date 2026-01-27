CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 -g
LDFLAGS = 

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
$(TARGET_CLI): $(LIB_OBJ) $(APP_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^

# Tests
$(TARGET_TEST): $(LIB_OBJ) $(TEST_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Static Library
$(TARGET_STATIC): $(LIB_OBJ) | $(LIB_OUT_DIR)
	ar rcs $@ $^

# Shared Library
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

test: $(TARGET_TEST)
	./$(TARGET_TEST)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET_CLI) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(TARGET_STATIC) $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET_SHARED) $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 include/cnotify.h $(DESTDIR)$(INCLUDEDIR)

.PHONY: all clean test install
