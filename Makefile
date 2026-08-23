# RevFS — Versioned Distributed File Storage System

CC      = clang
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g -Iinclude -pthread
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
test: dirs test_file test_chunk test_upload test_download test_version test_restore test_server test_client test_thread
	@echo ""
	@echo "Running Day 2 tests..."
	@./test_file
	@echo ""
	@echo "Running Day 3 tests..."
	@./test_chunk
	@echo ""
	@echo "Running Day 4 tests..."
	@./test_upload
	@echo ""
	@echo "Running Day 5 tests..."
	@./test_download
	@echo ""
	@echo "Running Day 6 tests..."
	@./test_version
	@echo ""
	@echo "Running Day 7 tests..."
	@./test_restore
	@echo ""
	@echo "Running Day 8 tests..."
	@./test_server
	@echo ""
	@echo "Running Day 9 tests..."
	@./test_client
	@echo ""
	@echo "Running Day 10 tests..."
	@./test_thread

test_file: tests/test_file.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_chunk: tests/test_chunk.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_upload: tests/test_upload.c src/upload.c src/thread.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_download: tests/test_download.c src/download.c src/upload.c src/thread.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_version: tests/test_version.c src/version.c src/upload.c src/thread.c src/download.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_restore: tests/test_restore.c src/restore.c src/version.c src/upload.c src/thread.c src/download.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_server: tests/test_server.c src/server.c src/thread.c src/restore.c src/version.c src/upload.c src/download.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_client: tests/test_client.c src/client.c src/server.c src/thread.c src/restore.c src/version.c src/upload.c src/download.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

test_thread: tests/test_thread.c src/thread.c src/client.c src/server.c src/restore.c src/version.c src/upload.c src/download.c src/chunk.c src/file.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf $(OBJ_DIR) $(BIN) test_file test_chunk test_upload test_download test_version test_restore test_server test_client test_thread
