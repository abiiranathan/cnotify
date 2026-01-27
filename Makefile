CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 -g
LDFLAGS = 

SRC_DIR = src
OBJ_DIR = build
BIN_DIR = bin

LIB_SRC = $(SRC_DIR)/cnotify.c
APP_SRC = $(SRC_DIR)/main.c

LIB_OBJ = $(OBJ_DIR)/cnotify.o
APP_OBJ = $(OBJ_DIR)/main.o

TARGET = $(BIN_DIR)/cnotify

all: $(TARGET)

$(TARGET): $(LIB_OBJ) $(APP_OBJ) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

.PHONY: all clean
