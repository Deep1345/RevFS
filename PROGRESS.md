# RevFS — Progress Tracker

> Versioned Distributed File Storage System built in C using POSIX APIs.
> 15-day milestone plan.

---

## ✅ Completed

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

---

## ⬜ Remaining

### Day 8 — TCP Server Skeleton
- `src/server.c` — TCP server with `socket()`, `bind()`, `listen()`, `accept()`
- Basic commands: PING, LIST

### Day 9 — Remote Upload/Download over TCP
- `src/client.c` — TCP client with `connect()`, `send()`, `recv()`
- Wire protocol for upload/download commands

### Day 10 — Concurrent Clients (pthreads)
- `src/thread.c` — Thread pool with `pthread_create()`, `pthread_join()`
- `pthread_mutex_t` for shared state protection

### Day 11 — Deduplication + Stats
- `src/dedup.c` — Track logical vs physical storage
- `./revfs stats` shows dedup savings

### Day 12 — Two-Node Replication
- `src/replication.c` — Write chunks to two nodes, fallback reads

### Day 13 — Write-Ahead Journaling + Crash Recovery
- `src/journal.c` — WAL with BEGIN/WRITE/COMMIT, rollback on crash

### Day 14 — Automated Tests + Demo Script
- `tests/test_*.c` — Comprehensive test suite
- `scripts/demo.sh` — End-to-end demo

### Day 15 — README + Cleanup + v1.0.0 Tag
- Polish README with architecture diagrams
- Code cleanup, final review
- `git tag v1.0.0`

---

## 📁 File Layout

```
revfs/
├── src/
│   ├── main.c          ← CLI entry point                    ✅ Day 1
│   ├── file.c          ← POSIX file abstraction             ✅ Day 2
│   ├── chunk.c         ← Chunking + SHA-256                 ✅ Day 3
│   ├── upload.c        ← Upload + metadata persistence      ✅ Day 4
│   ├── download.c      ← Download + reconstruct             ✅ Day 5
│   ├── version.c       ← Versioning                         ✅ Day 6
│   ├── restore.c       ← Restore                            ✅ Day 7
│   ├── server.c        ← TCP server                         ⬜ Day 8
│   ├── client.c        ← TCP client                         ⬜ Day 9
│   ├── thread.c        ← Thread pool                        ⬜ Day 10
│   ├── dedup.c         ← Deduplication + stats              ⬜ Day 11
│   ├── replication.c   ← Two-node replication               ⬜ Day 12
│   └── journal.c       ← WAL journaling                     ⬜ Day 13
├── include/
│   └── revfs.h         ← Global header                      ✅ Day 1+2+3+4+5+6+7
├── tests/
│   ├── test_file.c     ← Day 2 tests (8/8 pass)            ✅ Day 2
│   ├── test_chunk.c    ← Day 3 tests (10/10 pass)          ✅ Day 3
│   ├── test_upload.c   ← Day 4 tests (10/10 pass)          ✅ Day 4
│   ├── test_download.c ← Day 5 tests (10/10 pass)          ✅ Day 5
│   ├── test_version.c  ← Day 6 tests (10/10 pass)          ✅ Day 6
│   └── test_restore.c  ← Day 7 tests (10/10 pass)          ✅ Day 7
├── data/               ← Runtime chunk/metadata storage
├── docs/               ← Architecture docs
├── Makefile            ← Build system                        ✅ Day 1+2+3+4+5+6+7
├── README.md           ← Project docs                        ✅ Day 1
├── PROGRESS.md         ← This file
└── .gitignore                                                ✅ Day 1
```

---

## 🛠 Build & Test

```bash
make              # Build everything
make test         # Run all tests
make clean        # Clean build artifacts
./revfs --help    # CLI help
./revfs --version # Version info
```
