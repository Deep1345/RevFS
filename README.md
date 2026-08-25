# RevFS — Versioned Distributed File Storage System

A high-performance, crash-resilient **versioned distributed file storage system** built in **C** using raw **POSIX APIs** — featuring content-addressed chunking, SHA-256 deduplication, monotonic file versioning, non-destructive restore, multi-threaded TCP server/client architecture, two-node high-availability replication with transparent read failover, and Write-Ahead Log (WAL) journaling.

Zero external dependencies beyond standard C11, POSIX system calls, and macOS CommonCrypto.

---

## Key Capabilities

- **Content-Addressed Storage (CAS)**: Files are split into 4 MB chunks and stored by their SHA-256 digests in a two-character prefix directory hierarchy.
- **Global Deduplication**: Duplicate chunks across distinct files or versions are stored only once, maximizing storage efficiency.
- **Monotonic Versioning**: Non-destructive upload model where each upload generates a new version manifest, preserving full historical integrity.
- **Zero-Copy Restore**: Restoring an older version generates a new version manifest pointing to existing CAS chunks without disk re-duplication.
- **Concurrent Networking**: Multi-threaded TCP server leveraging a bounded ring-buffer thread pool with condition-variable synchronization.
- **High-Availability Replication**: Dual-node replication with configurable write quorum, transparent read failover, and bidirectional replica sync/repair.
- **Crash Recovery & Durability**: Transactional Write-Ahead Logging (WAL) with `fsync` guarantees and automatic rollback of uncommitted operations on restart.
- **Real-Time Storage Analytics**: Real-time inspection of logical vs. physical disk usage, unique chunks, and space savings ratio.

---

## Architecture Overview

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

### Storage Layout

```
data/
├── chunks/                    # Content-Addressed Store
│   ├── ab/                    # 2-character prefix subdirectories
│   │   └── abcdef01234567...  # SHA-256 chunk payload
│   └── fe/
│       └── fedcba98765432...
├── meta/                      # Line-oriented version metadata
│   ├── myfile.txt/
│   │   ├── v1.meta
│   │   ├── v2.meta
│   │   └── v3.meta
│   └── document.pdf/
│       └── v1.meta
└── journal.wal                # Write-Ahead Log
```

---

## Building

```bash
make          # Compiles the production binary: ./revfs
make clean    # Cleans object files and binaries
```

---

## Usage Reference

### Local Storage Operations

```bash
# Upload a file (creates version 1 or increments existing version)
./revfs upload document.pdf

# Download the latest version
./revfs download document.pdf output.pdf

# Download a specific historical version
./revfs download document.pdf output.pdf --version 1

# View complete version history
./revfs history document.pdf

# Restore a previous version non-destructively
./revfs restore document.pdf 1

# List all stored files
./revfs list

# Display deduplication & storage metrics
./revfs stats
```

### Remote Server & Client Mode

```bash
# Start multi-threaded server (default: port 9000, 4 worker threads)
./revfs server 9000 --threads 8

# Remote upload / download
./revfs upload myfile.txt --host 127.0.0.1 --port 9000
./revfs download myfile.txt output.txt --host 127.0.0.1 --port 9000 --version 1

# Remote inspection
./revfs history myfile.txt --host 127.0.0.1 --port 9000
./revfs list --host 127.0.0.1 --port 9000
./revfs stats --host 127.0.0.1 --port 9000
./revfs ping hello --host 127.0.0.1 --port 9000
```

### Two-Node Replication & Failover

```bash
# Start primary and secondary storage nodes
./revfs server 9000 &
./revfs server 9001 &

# Replicate file across both nodes (dual-write with quorum)
./revfs upload document.pdf --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Download with transparent failover if primary is down
./revfs download document.pdf out.pdf --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Synchronize and auto-repair missing chunks/manifests between replicas
./revfs sync --primary 127.0.0.1:9000 --replica 127.0.0.1:9001

# Inspect cluster health
./revfs repl-status --primary 127.0.0.1:9000 --replica 127.0.0.1:9001
```

---

## Wire Protocol Specification

RevFS communicates using a high-efficiency line-oriented text protocol over TCP:

| Command | Response | Description |
|---------|----------|-------------|
| `PING [msg]` | `PONG [msg]\n` | Connection liveness probe |
| `INFO` | `OK RevFS <ver> ...\n` | Server details and configuration |
| `LIST` | `OK <N> files\n...\nEND\n` | Remote file catalog |
| `HISTORY <file>` | `OK <N> versions\n...\nEND\n` | File version history |
| `STATS` | `OK <metrics>\n` | Storage and deduplication statistics |
| `HAS_CHUNK <hash>` | `YES\n` or `NO\n` | Check chunk presence in CAS |
| `STORE_CHUNK <hash> <size>\n<data>` | `OK\n` | Store raw chunk payload |
| `GET_CHUNK <hash>` | `OK <size>\n<data>` | Retrieve raw chunk payload |
| `GET_META <file> <ver>` | `OK ...\nEND\n` | Fetch structured version metadata |
| `UPLOAD_META ...\nEND` | `OK <ver>\n` | Commit version metadata manifest |
| `HELP` | Command reference | Supported wire commands |
| `QUIT` | `BYE\n` | Terminate TCP session |

---

## Codebase Organization

```
revfs/
├── src/
│   ├── main.c          # CLI entry point and argument parsing
│   ├── file.c          # POSIX system call abstractions (EINTR safety, pread/pwrite)
│   ├── chunk.c         # Content-addressed chunk storage & SHA-256 hashing
│   ├── upload.c        # Upload pipeline & metadata serialization
│   ├── download.c      # Download pipeline & chunk reassembly
│   ├── version.c       # Version history inspection & catalog listing
│   ├── restore.c       # Non-destructive version restore engine
│   ├── server.c        # Multi-threaded TCP socket server
│   ├── client.c        # TCP client with distributed deduplication
│   ├── thread.c        # Generic POSIX worker thread pool
│   ├── dedup.c         # Storage metrics & deduplication analytics
│   ├── replication.c   # Two-node replication, failover & sync
│   └── journal.c       # Write-Ahead Log (WAL) & crash recovery
├── include/
│   └── revfs.h         # Unified header (types, structures, function prototypes)
├── scripts/
│   └── demo.sh         # End-to-end demonstration script
├── Makefile            # Build system
├── README.md           # Documentation
└── .gitignore
```

---

## End-to-End Demo

Run the demonstration script to verify all features locally:

```bash
./scripts/demo.sh
```
