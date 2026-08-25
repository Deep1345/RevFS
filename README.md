# RevFS — Versioned Distributed File Storage System

A production-grade **versioned distributed file storage system** built entirely in **C** using **POSIX APIs** — with zero external dependencies beyond the C standard library, POSIX, and macOS CommonCrypto.

RevFS provides content-addressed chunk storage with SHA-256 deduplication, file versioning with non-destructive restore, a multi-threaded TCP server/client architecture, two-node replication with automatic failover, write-ahead journaling for crash recovery, and comprehensive storage analytics.

---

## Features

| Feature | Description |
|---------|-------------|
| **Content-Addressed Storage** | Files split into 4 MB chunks, each stored by its SHA-256 hash |
| **Natural Deduplication** | Identical chunks across any files or versions are stored only once |
| **File Versioning** | Every upload creates a new version; full history is preserved |
| **Non-Destructive Restore** | Restoring a version creates a new version pointing to existing chunks |
| **TCP Server/Client** | Line-oriented wire protocol for remote operations |
| **Multi-Threaded Server** | POSIX thread pool with configurable worker count |
| **Two-Node Replication** | Dual-write with automatic read failover and two-way sync |
| **Write-Ahead Journal** | WAL-based crash recovery with transactional semantics |
| **Storage Analytics** | Deduplication ratio, space savings, and chunk statistics |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         CLI (main.c)                            │
│     upload │ download │ history │ restore │ list │ stats │ …    │
├─────────────┬────────────────┬──────────────────────────────────┤
│  Local Ops  │  Remote Ops    │  Replicated Ops                  │
│  upload.c   │  client.c      │  replication.c                   │
│  download.c │  server.c      │  (primary + secondary nodes)     │
│  version.c  │  thread.c      │                                  │
│  restore.c  │  (thread pool) │                                  │
├─────────────┴────────────────┴──────────────────────────────────┤
│                    Core Storage Engine                           │
│  ┌──────────┐  ┌──────────────┐  ┌───────────┐  ┌───────────┐  │
│  │ chunk.c  │  │  upload.c    │  │  dedup.c  │  │ journal.c │  │
│  │ SHA-256  │  │  metadata    │  │  stats    │  │ WAL/crash │  │
│  │ CAS      │  │  versioning  │  │  analysis │  │ recovery  │  │
│  └──────────┘  └──────────────┘  └───────────┘  └───────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                   POSIX File Layer (file.c)                      │
│   open │ read │ write │ pread │ pwrite │ seek │ fsync │ mkdir   │
└─────────────────────────────────────────────────────────────────┘
```

### Data Layout

```
data/
├── chunks/                    # Content-Addressed Store
│   ├── ab/                    # 2-char prefix directories
│   │   └── abcdef01234567...  # Chunk file (named by SHA-256 hash)
│   └── fe/
│       └── fedcba98765432...
├── meta/                      # Metadata Store
│   ├── myfile.txt/
│   │   ├── v1.meta            # Version 1 manifest
│   │   ├── v2.meta            # Version 2 manifest
│   │   └── v3.meta
│   └── another.bin/
│       └── v1.meta
└── journal.wal                # Write-Ahead Log
```

---

## Build

**Requirements:** macOS with Xcode Command Line Tools (provides `clang` and CommonCrypto).

```bash
make              # Build the revfs binary
make test         # Run all 118 automated tests
make clean        # Remove build artifacts
```

---

## Usage

### Local Operations

```bash
# Upload a file (creates version 1, or increments version)
./revfs upload myfile.txt

# Download the latest version
./revfs download myfile.txt output.txt

# Download a specific version
./revfs download myfile.txt output.txt --version 2

# View version history
./revfs history myfile.txt

# Restore a previous version (non-destructive, creates new version)
./revfs restore myfile.txt 1

# List all stored files
./revfs list

# View storage and deduplication statistics
./revfs stats
```

### Server / Client

```bash
# Start a multi-threaded server (default: port 9000, 4 threads)
./revfs server
./revfs server 8080 --threads 8

# Remote operations
./revfs upload myfile.txt --host 192.168.1.10 --port 9000
./revfs download myfile.txt output.txt --host 192.168.1.10
./revfs history myfile.txt --host 192.168.1.10
./revfs list --host 192.168.1.10
./revfs stats --host 192.168.1.10
./revfs ping hello --host 192.168.1.10
```

### Replication

```bash
# Start two server nodes
./revfs server 9000 &    # Primary
./revfs server 9001 &    # Secondary

# Upload with replication (dual-write)
./revfs upload myfile.txt --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Download with automatic failover
./revfs download myfile.txt output.txt --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Two-way sync and auto-repair
./revfs sync --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Check cluster health
./revfs repl-status --primary 127.0.0.1:9000 --replica 127.0.0.1:9001
```

---

## Wire Protocol

RevFS uses a line-oriented text protocol over TCP:

| Command | Response | Description |
|---------|----------|-------------|
| `PING [msg]` | `PONG [msg]\n` | Health check |
| `INFO` | `OK RevFS <ver> ...\n` | Server information |
| `LIST` | `OK <N> files\n...\nEND\n` | List stored files |
| `HISTORY <file>` | `OK <N> versions\n...\nEND\n` | Version history |
| `STATS` | `OK <metrics>\n` | Storage statistics |
| `HAS_CHUNK <hash>` | `YES\n` or `NO\n` | Check chunk existence |
| `STORE_CHUNK <hash> <size>\n<data>` | `OK\n` | Store chunk data |
| `GET_CHUNK <hash>` | `OK <size>\n<data>` | Retrieve chunk data |
| `GET_META <file> <ver>` | `OK ...\nEND\n` | Get file metadata |
| `UPLOAD_META ...\nEND` | `OK <ver>\n` | Upload file metadata |
| `HELP` | Command listing | Available commands |
| `QUIT` | `BYE\n` | Close connection |

---

## Design Decisions

- **Zero External Dependencies**: Uses only POSIX APIs (`open`, `read`, `write`, `socket`, `pthread`, …) and macOS CommonCrypto for SHA-256. No third-party libraries.
- **Content-Addressed Storage (CAS)**: Files are chunked and stored by SHA-256 hash. Identical content is naturally deduplicated across all files and versions.
- **Atomic Writes**: All disk writes use a temp file + `fsync()` + `rename()` pattern to prevent partial/corrupt files on crash.
- **Thread-Safe Metadata**: A global metadata mutex ensures atomic version increments during concurrent uploads. Per-thread unique temp file suffixes prevent write races.
- **Non-Destructive Versioning**: Restoring a version creates a new version entry. No data is ever deleted or overwritten.
- **Write-Ahead Logging**: Transactional WAL (BEGIN/WRITE/COMMIT/ABORT) with automatic rollback of uncommitted transactions on recovery.
- **Two-Char Prefix Directories**: Chunk storage uses `chunks/ab/<hash>` layout to avoid filesystem degradation with millions of files in a single directory.
- **Heap-Allocated Metadata**: The `revfs_meta_t` struct (~260 KB) is always heap-allocated to prevent stack overflow.

---

## File Layout

```
revfs/
├── src/
│   ├── main.c          ← CLI entry point + argument parsing
│   ├── file.c          ← POSIX file abstraction (16 functions)
│   ├── chunk.c         ← Chunking + SHA-256 CAS
│   ├── upload.c        ← Upload pipeline + metadata persistence
│   ├── download.c      ← Download + file reconstruction
│   ├── version.c       ← Version history + file listing
│   ├── restore.c       ← Non-destructive version restore
│   ├── server.c        ← Multi-threaded TCP server
│   ├── client.c        ← TCP client + remote operations
│   ├── thread.c        ← POSIX thread pool
│   ├── dedup.c         ← Deduplication statistics
│   ├── replication.c   ← Two-node replication + failover
│   └── journal.c       ← Write-ahead journaling + crash recovery
├── include/
│   └── revfs.h         ← Global header (types, constants, prototypes)
├── tests/
│   ├── test_file.c     ← File abstraction tests        (8 tests)
│   ├── test_chunk.c    ← Chunking + CAS tests          (10 tests)
│   ├── test_upload.c   ← Upload + metadata tests       (10 tests)
│   ├── test_download.c ← Download tests                (10 tests)
│   ├── test_version.c  ← Versioning tests              (10 tests)
│   ├── test_restore.c  ← Restore tests                 (10 tests)
│   ├── test_server.c   ← TCP server tests              (10 tests)
│   ├── test_client.c   ← TCP client tests              (10 tests)
│   ├── test_thread.c   ← Thread pool tests             (10 tests)
│   ├── test_dedup.c    ← Deduplication tests            (10 tests)
│   ├── test_replication.c ← Replication tests           (10 tests)
│   └── test_journal.c  ← WAL journal tests             (10 tests)
├── scripts/
│   └── demo.sh         ← End-to-end demo script
├── data/               ← Runtime chunk/metadata storage (gitignored)
├── Makefile            ← Build system
├── PROGRESS.md         ← Development progress tracker
├── README.md           ← This file
└── .gitignore
```

---

## Tests

118 automated tests across 12 test suites:

```bash
make test
```

```
━━━ RevFS Day 2  — POSIX File Tests           ━━━  8/8   pass
━━━ RevFS Day 3  — Chunking + SHA-256 Tests    ━━━  10/10 pass
━━━ RevFS Day 4  — Upload + Metadata Tests     ━━━  10/10 pass
━━━ RevFS Day 5  — Download Tests              ━━━  10/10 pass
━━━ RevFS Day 6  — Versioning Tests            ━━━  10/10 pass
━━━ RevFS Day 7  — Restore Tests               ━━━  10/10 pass
━━━ RevFS Day 8  — TCP Server Tests            ━━━  10/10 pass
━━━ RevFS Day 9  — TCP Client Tests            ━━━  10/10 pass
━━━ RevFS Day 10 — Thread Pool Tests           ━━━  10/10 pass
━━━ RevFS Day 11 — Dedup + Stats Tests         ━━━  10/10 pass
━━━ RevFS Day 12 — Replication Tests           ━━━  10/10 pass
━━━ RevFS Day 13 — Journal Tests               ━━━  10/10 pass
```

---

## Demo

Run the end-to-end demo to see all features in action:

```bash
./scripts/demo.sh
```

---

## License

This project was built as a systems programming exercise in distributed storage.
