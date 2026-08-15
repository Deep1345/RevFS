# RevFS — Versioned Distributed File Storage System

CC      = clang
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g -Iinclude
LDFLAGS =

SRC_DIR = src
OBJ_DIR = obj
BIN     = revfs

# Collect all source files
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean dirs test

all: dirs $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

dirs:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p data

# -------- Tests --------
test: dirs test_file
	@echo ""
	@echo "Running Day 2 tests..."
	@./test_file

test_file: tests/test_file.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(OBJ_DIR) $(BIN) test_file

