# RevFS — Progress Tracker

> Versioned Distributed File Storage System built in C using POSIX APIs.
> 15-day milestone plan.

---

## ✅ Completed (Days 1–15)

### Day 1 — Project Skeleton + CLI
**Files:**
- `include/revfs.h` — Global header with constants (`REVFS_VERSION`, `REVFS_CHUNK_SIZE`, `REVFS_DEFAULT_PORT`, `REVFS_DATA_DIR`)
- `src/main.c` — CLI entry point with `--help`, `--version`, and stubs for all commands
- `Makefile` — Build system (clang, `-Wall -Wextra -Wpedantic -std=c11`)
- `.gitignore` — Ignores `obj/`, binaries, `data/`, editor files
- `README.md` — Project readme

**CLI commands registered:** `upload`, `download`, `history`, `restore`, `list`, `stats` (all stubs)

---

### Day 2 — POSIX File Abstraction
**Files:**
- `src/file.c` — 16 functions wrapping raw POSIX syscalls (no stdio)
- `tests/test_file.c` — 8 automated tests (all passing)

**Functions implemented:**
| Function | Wraps | Purpose |
|----------|-------|---------|
| `revfs_file_open()` | `open()` | Open/create files |
| `revfs_file_close()` | `close()` | Close fd |
| `revfs_file_read()` | `read()` | Read bytes, EINTR-safe |
| `revfs_file_read_all()` | `read()` | Read exact count, loops on short reads |
| `revfs_file_write()` | `write()` | Write bytes, EINTR-safe |
| `revfs_file_write_all()` | `write()` | Write exact count, loops on short writes |
| `revfs_file_pread()` | `pread()` | Positional read (thread-safe) |
| `revfs_file_pwrite()` | `pwrite()` | Positional write (thread-safe) |
| `revfs_file_seek()` | `lseek()` | Reposition file offset |
| `revfs_file_size()` | `fstat()` | Size of open file |
| `revfs_file_size_path()` | `stat()` | Size by path |
| `revfs_file_exists()` | `stat()` | Check existence |
| `revfs_file_sync()` | `fsync()` | Flush to disk |
| `revfs_file_append()` | combined | Open+write+sync+close |
| `revfs_mkdir_p()` | `mkdir()` | Recursive mkdir |

---

### Day 3 — Chunking + SHA-256 Content Addressing
**Files:**
- `src/chunk.c` — Content-addressed chunk storage with SHA-256 hashing
- `tests/test_chunk.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_sha256()` | SHA-256 hash of in-memory data → 64-char hex string |
| `revfs_sha256_fd()` | SHA-256 hash of an open file descriptor (streaming) |
| `revfs_chunk_store_path()` | Build content-addressed path: `data/chunks/ab/<hash>` |
| `revfs_chunk_store()` | Hash data, store chunk atomically (skip if dedup) |
| `revfs_chunk_load()` | Load a chunk by its hash from the store |
| `revfs_chunk_exists()` | Check if a chunk hash exists in the store |
| `revfs_file_chunk()` | Split a file into 4 MB chunks, store each, return hashes |
| `revfs_chunks_reassemble()` | Reassemble a file from ordered chunk hashes |

**Key design decisions:**
- Uses Apple CommonCrypto (macOS) for SHA-256 — zero external dependencies
- Atomic writes via temp file + rename to prevent partial chunks on crash
- Two-char prefix directories (`data/chunks/ab/`) to avoid FS performance issues
- Natural deduplication: if hash exists → skip write, return 1

---

### Day 4 — Upload + Metadata Persistence
**Files:**
- `src/upload.c` — Upload pipeline + metadata read/write + version tracking
- `tests/test_upload.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_upload()` | Main entry: validate → chunk → store → version → write metadata |
| `revfs_meta_write()` | Write metadata atomically (temp + rename) to `data/meta/<name>/v<N>.meta` |
| `revfs_meta_read()` | Parse metadata file, supports reading latest version (version=-1) |
| `revfs_meta_next_version()` | Scan metadata dir for existing versions, return next number |
| `revfs_meta_list_files()` | List all uploaded files by scanning `data/meta/` subdirectories |

**Metadata format** (line-oriented key=value, zero external dependencies):
```
name=myfile.txt
version=1
chunks=3
size=12582912
timestamp=1723886400
hash.0=abcdef0123...
hash.1=fedcba9876...
hash.2=1234567890...
```

**Key design decisions:**
- Metadata stored as flat key=value files — no JSON library needed in C
- Atomic writes via temp file + rename (crash-safe, same pattern as chunk store)
- Version numbering by scanning `data/meta/<filename>/` for `v<N>.meta` files
- `revfs_meta_t` struct uses `REVFS_META_MAX_CHUNKS` (4096) to stay heap-friendly
- Heap allocation for chunk hashes during upload (avoids stack overflow)

---

### Day 5 — Download + File Reconstruction
**Files:**
- `src/download.c` — Download pipeline: metadata → chunk verification → reassembly
- `tests/test_download.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_download()` | Main entry: read metadata → verify chunks → reassemble → verify size |

**Download pipeline:**
1. Read metadata for the requested filename and version (or latest if version == -1)
2. Verify all chunk hashes referenced in metadata exist in the content-addressed store
3. Reassemble the original file from the ordered chunk list via `revfs_chunks_reassemble()`
4. Verify reconstructed file size matches metadata record
5. Clean up partial output file on any error (no partial files left behind)

**Key design decisions:**
- Uses heap-allocated `revfs_meta_t` (struct is ~260 KB) to avoid stack overflow
- Validates chunk existence before attempting reassembly (fail-fast)
- Cleans up partial output files on error (no dangling partial downloads)
- Supports both specific version download and latest version (version == -1)
- CLI supports optional `--version N` flag: `./revfs download <file> <output> [--version N]`

---

### Day 6 — File Versioning
**Files:**
- `src/version.c` — Version history listing + file enumeration
- `tests/test_version.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_version_count()` | Count how many versions exist for a filename |
| `revfs_version_list()` | Read all version metadata into a caller-supplied array |
| `revfs_history()` | Print full version history for a file to stdout |
| `revfs_list_files()` | List all files stored in RevFS with version counts |

**CLI commands wired up:**
- `./revfs history <file>` — Shows all versions with size, chunk count, timestamp, and latest marker
- `./revfs list` — Lists all stored files with version counts and latest file size

**Key design decisions:**
- Builds entirely on Day 4 metadata infrastructure (`revfs_meta_read`, `revfs_meta_next_version`, `revfs_meta_list_files`)
- Human-readable size formatting (B/KB/MB) and timestamp formatting in output
- `revfs_version_list()` exposes structured data for programmatic use (used by tests)
- Gracefully skips corrupt/missing version files during enumeration
- Heap-allocated `revfs_meta_t` for history display (~260 KB per struct)

### Day 7 — Version Restore
**Files:**
- `src/restore.c` — Non-destructive version restore
- `tests/test_restore.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_restore()` | Restore a specific version by creating a new version copying old metadata |

**CLI commands wired up:**
- `./revfs restore <file> <version>` — Restores `<file>` to `<version>` by generating a new version pointing to the existing content chunks

**Key design decisions:**
- Non-destructive: older versions are preserved, creating a new monotonic version entry (e.g. restoring v1 when latest is v3 creates v4)
- Zero data re-copying: leverages content-addressed chunk deduplication, sharing chunk hashes directly
- Chunk validation: verifies chunk availability in CAS before writing the new metadata record
- Prevents redundant restores when target version is already the latest version

### Day 8 — TCP Server Skeleton
**Files:**
- `src/server.c` — TCP server with `socket()`, `bind()`, `listen()`, `accept()`, `poll()`
- `tests/test_server.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_server_create()` | Create, configure (`SO_REUSEADDR`), bind, and listen on TCP socket |
| `revfs_server_start()` | Start server accept loop on specified port with graceful signal shutdown |
| `revfs_server_stop()` | Signal running server loop to terminate cleanly |
| `revfs_server_handle_client()` | Read line-delimited commands from client until disconnect or `QUIT` |
| `revfs_server_process_command()`| Parse and dispatch command (`PING`, `LIST`, `INFO`, `HISTORY`, `HELP`, `QUIT`) |

**Wire protocol implemented:**
- `PING [msg]` → `PONG [msg]\n`
- `INFO` → `OK RevFS <version> (chunk_size=4194304, default_port=9000)\n`
- `LIST` → `OK <N> files\n<filename> <version_count> <size>\n...\nEND\n`
- `HISTORY <file>` → `OK <N> versions\nv<ver> <size> <chunks> <timestamp>\n...\nEND\n`
- `HELP` → `OK Supported commands: PING, LIST, INFO, HISTORY <file>, HELP, QUIT\n`
- `QUIT` / `EXIT` → `BYE\n` (closes socket connection)
- Error response → `ERR <reason>\n`

**CLI commands wired up:**
- `./revfs server [port]` — Start TCP server (default: port 9000)

**Key design decisions:**
- Uses standard POSIX sockets (`<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<poll.h>`) with zero external dependencies
- Non-blocking signal-safe event loop with `poll()` timeout (500ms) for responsive `SIGINT`/`SIGTERM` shutdown
- `SIGPIPE` ignored to prevent process termination on client disconnect
- Line-buffered stream processing handling fragmented or concatenated TCP packets
- Ephemeral port binding (`port 0`) support via `getsockname()` for isolated automated testing

---

### Day 9 — Remote Upload/Download over TCP
**Files:**
- `src/client.c` — TCP client with `connect()`, `send()`, `recv()`, chunk CAS sync & high-level CLI commands
- `src/server.c` — Extended wire protocol handlers (`HAS_CHUNK`, `STORE_CHUNK`, `GET_CHUNK`, `GET_META`, `UPLOAD_META`)
- `tests/test_client.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_client_connect()` | Establish TCP connection to remote host and port |
| `revfs_client_disconnect()` | Send `QUIT` and cleanly close socket |
| `revfs_client_ping()` | Send `PING` and receive `PONG` response |
| `revfs_client_has_chunk()` | Query if remote CAS already contains a chunk by SHA-256 |
| `revfs_client_store_chunk()` | Stream chunk binary payload to remote CAS with hash verification |
| `revfs_client_get_chunk()` | Download chunk payload from remote server with integrity verification |
| `revfs_client_get_meta()` | Fetch structured version metadata manifest from remote server |
| `revfs_client_upload_meta()` | Submit version metadata manifest to finalize remote upload |
| `revfs_client_upload()` | High-level pipeline: chunking → remote deduplication → missing chunk upload → metadata commit |
| `revfs_client_download()` | High-level pipeline: fetch metadata → CAS cache check / remote chunk fetch → reassembly |
| `revfs_client_list()` | Query and display formatted remote file catalog |
| `revfs_client_history()` | Query and display formatted version history on remote server |

**CLI commands wired up:**
- `./revfs upload <file> [--host H] [--port P]` — Upload file to remote RevFS server
- `./revfs download <file> <output> [--version N] [--host H] [--port P]` — Download file from remote RevFS server
- `./revfs history <file> [--host H] [--port P]` — View remote file version history
- `./revfs list [--host H] [--port P]` — List remote files
- `./revfs ping [msg] [--host H] [--port P]` — Ping remote RevFS server

**Key design decisions:**
- Distributed Content-Addressed Storage deduplication: client queries `HAS_CHUNK` before sending chunk payload over network, saving bandwidth on redundant chunks
- Local CAS caching on download: chunks retrieved from remote server are cached locally in CAS for fast subsequent access
- Atomic download reassembly via `.tmp` file + rename with size and SHA-256 checksum validation
- POSIX-standard socket handling supporting DNS resolution (`gethostbyname`) and IPv4 (`inet_pton`)

---

### Day 10 — Concurrent Clients & POSIX Thread Pool
**Files:**
- `src/thread.c` — Reusable POSIX thread pool with ring-buffer queue, mutex & condition variables
- `src/server.c` — Multi-threaded server accept loop dispatching client sessions to worker threads
- `src/upload.c` & `src/restore.c` — Thread-safe metadata locks and collision-free atomic temporary files
- `src/chunk.c` — Thread-safe unique temporary file paths for concurrent CAS chunk writes
- `src/main.c` — Support `--threads <N>` argument in `./revfs server`
- `tests/test_thread.c` — 10 automated concurrency tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_tpool_create()` | Allocate and initialize worker threads, task circular buffer, mutex & condvars |
| `revfs_tpool_submit()` | Enqueue task for execution, blocking if task buffer is full |
| `revfs_tpool_try_submit()` | Non-blocking task enqueueing (returns `EAGAIN` if queue is full) |
| `revfs_tpool_wait()` | Barrier synchronization: block until all queued and active tasks complete |
| `revfs_tpool_destroy()` | Graceful (`wait_for_tasks=1`) or immediate (`wait_for_tasks=0`) worker shutdown & join |
| `revfs_tpool_active_workers()` | Query count of worker threads currently processing tasks |
| `revfs_tpool_queue_count()` | Query count of tasks currently waiting in queue buffer |
| `revfs_lock_meta()` | Acquire global metadata mutex lock for safe atomic version increments |
| `revfs_unlock_meta()` | Release global metadata mutex lock |
| `revfs_server_start_threaded()` | Start multi-threaded server accepting and serving concurrent clients via worker pool |

**CLI commands wired up:**
- `./revfs server [port] [--threads N]` — Start multi-threaded TCP server (default: port 9000, 4 worker threads)

**Key design decisions:**
- Worker pool architecture: bounded circular ring buffer with condition variables (`notify_not_empty`, `notify_not_full`, `notify_idle`) preventing unbounded memory usage under load
- Non-blocking server accept loop: client sockets are accepted and handed off immediately to pool workers via heap-allocated connection context, freeing the main thread to accept next connections
- Metadata race prevention: `revfs_lock_meta()` ensures atomic version increments during concurrent uploads or version restores across threads
- Collision-free temp files: chunk stores and metadata writes use thread-safe unique suffixes (`.tmp.<pid>_<tid>`) preventing concurrent worker write races before atomic POSIX renames
- Graceful shutdown: `SIGINT`/`SIGTERM` or `revfs_server_stop()` drains running client requests through `revfs_tpool_destroy(pool, 1)` and cleanly closes listening sockets

### Day 11 — Deduplication + Storage Stats
**Files:**
- `src/dedup.c` — Storage metrics calculator, CAS directory scanner & statistics printer
- `src/server.c` — Extended wire protocol with `STATS` command
- `src/client.c` — TCP client stats queries (`revfs_client_get_stats`, `revfs_client_stats`)
- `src/main.c` — Wire up `./revfs stats [--host H] [--port P]` CLI command
- `tests/test_dedup.c` — 10 automated unit & integration tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_stats_chunks_info()` | Scan `data/chunks/` to count total stored chunks and physical disk usage |
| `revfs_stats_calculate()` | Compute logical bytes, physical CAS usage, unique chunks, dedup ratio & savings |
| `revfs_stats_print()` | Display formatted storage and deduplication statistics report table |
| `revfs_stats()` | Calculate and display local storage statistics to stdout |
| `revfs_client_get_stats()` | Query structured `revfs_stats_t` storage metrics from remote server over TCP |
| `revfs_client_stats()` | Connect to remote server, fetch stats, and display formatted report |

**Wire protocol implemented:**
- `STATS` → `OK <files> <versions> <logical_bytes> <physical_bytes> <unique_chunks> <ref_chunks> <ratio> <savings_bytes> <savings_pct>\n`

**CLI commands wired up:**
- `./revfs stats` — Display storage and deduplication statistics for local repository
- `./revfs stats [--host H] [--port P]` — Display storage statistics for remote RevFS server

**Key design decisions:**
- Accurate deduplication analysis: evaluates active manifests and calculates deduplication ratio ($\text{logical} / \text{physical}$) based on unique chunk references and physical bytes
- DJB2 hash-set tracking: $O(1)$ amortized unique chunk membership lookup without external dependencies
- Safe floating-point handling: prevents division-by-zero on empty repositories, defaulting cleanly to `1.00x` ratio and `0.0%` savings
- Human-readable unit formatting (B, KB, MB, GB) with aligned ASCII box tables

---

### Day 12 — Two-Node Replication & Failover
**Files:**
- `src/replication.c` — Dual-node chunk/metadata writes, transparent read failover, degraded mode & sync/repair
- `src/main.c` — Wire up `--primary` and `--replica` flags for `upload`, `download`, `sync`, and `repl-status`
- `tests/test_replication.c` — 10 automated replication and failover tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_repl_config_init()` | Initialize replication cluster configuration with primary/secondary endpoints |
| `revfs_repl_ping()` | Check live health and reachability of both replication cluster nodes |
| `revfs_repl_has_chunk()` | Query chunk presence across both primary and secondary nodes |
| `revfs_repl_store_chunk()` | Dual-write chunk payload to both nodes honoring write quorum policy |
| `revfs_repl_get_chunk()` | Fetch chunk from primary with automatic failover to secondary on error/corruption |
| `revfs_repl_upload()` | High-level replicated upload: chunking → dual CAS sync → dual metadata commit |
| `revfs_repl_download()` | High-level failover download: fallback metadata fetch → fallback chunk pull → reassembly |
| `revfs_repl_sync()` | Two-way replica synchronization and auto-repair copying missing chunks/manifests |
| `revfs_repl_list()` | List catalog from primary node, falling back to secondary if primary is down |
| `revfs_repl_history()` | View file history from primary node, falling back to secondary if primary is down |

**CLI commands wired up:**
- `./revfs upload <file> --primary <H:P> --replica <H:P>` — Replicate file across both storage nodes
- `./revfs download <file> <output> [--version N] --primary <H:P> --replica <H:P>` — Download with transparent node failover
- `./revfs sync --primary <H:P> --replica <H:P>` — Run two-way sync and repair between nodes
- `./revfs repl-status --primary <H:P> --replica <H:P>` — Inspect online/offline status of cluster nodes

**Key design decisions:**
- Transparent client-side failover: download operations automatically fail over to the secondary replica if the primary is offline, or if an individual chunk on the primary fails SHA-256 integrity verification
- Configurable write quorum: supports degraded write mode (`quorum=1`) allowing uploads to succeed even when one replica node is down
- Pure thread-safe POSIX `safe_basename`: eliminates libc `basename(3)` internal static buffer race conditions during concurrent operations
- Two-way auto-repair: `revfs_repl_sync` bidirectional reconciliation detects and synchronizes missing chunks and version manifests between nodes

---

### Day 13 — Write-Ahead Journaling + Crash Recovery
**Files:**
- `src/journal.c` — WAL journaling with crash recovery
- `tests/test_journal.c` — 10 automated tests (all passing)

**Functions implemented:**
| Function | Purpose |
|----------|---------|
| `revfs_journal_open()` | Open/create WAL file, auto-recover if pending transactions exist |
| `revfs_journal_close()` | Sync and close WAL; abort any active transaction |
| `revfs_journal_begin()` | Start a new transaction, write `BEGIN <txn_id>` record |
| `revfs_journal_write()` | Record an intended write operation (`WRITE <txn_id> <path> <size>`) |
| `revfs_journal_commit()` | Mark transaction as committed (`COMMIT <txn_id>`), fsync |
| `revfs_journal_abort()` | Abort transaction, clean up write targets, write `ABORT` record |
| `revfs_journal_recover()` | Replay WAL: rollback uncommitted transactions, preserve committed ones |
| `revfs_journal_status()` | Query active transaction state and next transaction ID |

**Key design decisions:**
- WAL uses line-oriented text records (BEGIN/WRITE/COMMIT/ABORT) for human-readable debugging
- Every WAL record is followed by `fsync()` for durability guarantees
- Uncommitted transactions are automatically rolled back on next `revfs_journal_open()`
- Old WAL is backed up to `journal.wal.bak` before starting fresh after recovery
- Thread-safe via `pthread_mutex_t` protecting all WAL operations
- Heap-allocated recovery state to prevent stack overflow on large WAL files

---

### Day 14 — Demo Script
**Files:**
- `scripts/demo.sh` — End-to-end demo exercising all RevFS features

### Day 15 — README + Cleanup + v1.0.0 Tag
**Files:**
- `README.md` — Comprehensive README with architecture, usage, wire protocol, design decisions
- `PROGRESS.md` — Finalized progress tracker

---

## 📁 File Layout

```
revfs/
├── src/
│   ├── main.c          ← CLI entry point                    ✅ Day 1+8+9+10+11+12
│   ├── file.c          ← POSIX file abstraction             ✅ Day 2
│   ├── chunk.c         ← Chunking + SHA-256                 ✅ Day 3
│   ├── upload.c        ← Upload + metadata persistence      ✅ Day 4+10+12
│   ├── download.c      ← Download + reconstruct             ✅ Day 5
│   ├── version.c       ← Versioning                         ✅ Day 6
│   ├── restore.c       ← Restore                            ✅ Day 7
│   ├── server.c        ← TCP server (multi-threaded)        ✅ Day 8+9+10+11
│   ├── client.c        ← TCP client                         ✅ Day 9+11+12
│   ├── thread.c        ← Thread pool & synchronization      ✅ Day 10
│   ├── dedup.c         ← Deduplication + stats              ✅ Day 11
│   ├── replication.c   ← Two-node replication               ✅ Day 12
│   └── journal.c       ← WAL journaling + crash recovery    ✅ Day 13
├── include/
│   └── revfs.h         ← Global header                      ✅ Day 1-13
├── tests/
│   ├── test_file.c     ← Day 2 tests (8/8 pass)            ✅ Day 2
│   ├── test_chunk.c    ← Day 3 tests (10/10 pass)          ✅ Day 3
│   ├── test_upload.c   ← Day 4 tests (10/10 pass)          ✅ Day 4
│   ├── test_download.c ← Day 5 tests (10/10 pass)          ✅ Day 5
│   ├── test_version.c  ← Day 6 tests (10/10 pass)          ✅ Day 6
│   ├── test_restore.c  ← Day 7 tests (10/10 pass)          ✅ Day 7
│   ├── test_server.c   ← Day 8 tests (10/10 pass)          ✅ Day 8
│   ├── test_client.c   ← Day 9 tests (10/10 pass)          ✅ Day 9
│   ├── test_thread.c   ← Day 10 tests (10/10 pass)         ✅ Day 10
│   ├── test_dedup.c    ← Day 11 tests (10/10 pass)         ✅ Day 11
│   ├── test_replication.c ← Day 12 tests (10/10 pass)      ✅ Day 12
│   └── test_journal.c  ← Day 13 tests (10/10 pass)         ✅ Day 13
├── scripts/
│   └── demo.sh         ← End-to-end demo                    ✅ Day 14
├── data/               ← Runtime chunk/metadata storage
├── docs/               ← Architecture docs
├── Makefile            ← Build system                        ✅ Day 1-13
├── README.md           ← Project docs                        ✅ Day 15
├── PROGRESS.md         ← Progress tracker                    ✅ Day 1-15
└── .gitignore                                                ✅ Day 1+10
```

---

## 🛠 Build & Test

```bash
make              # Build everything (binary: revfs)
make test         # Run all 118 tests across Days 2-13
make clean        # Clean build artifacts
./revfs --help    # CLI help
./revfs stats     # Storage and deduplication statistics
```

