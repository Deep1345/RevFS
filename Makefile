# RevFS — Versioned Distributed File Storage System

CC      = clang
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g -Iinclude -pthread
LDFLAGS =

SRC_DIR = src
OBJ_DIR = obj
BIN     = revfs

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean dirs

all: dirs $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

dirs:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p data

clean:
	rm -rf $(OBJ_DIR) $(BIN)
