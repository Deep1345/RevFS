#ifndef REVFS_H
#define REVFS_H

#define REVFS_VERSION "0.1.0"
#define REVFS_NAME    "RevFS"

/* Default chunk size: 4 MB */
#define REVFS_CHUNK_SIZE (4 * 1024 * 1024)

/* Default server port */
#define REVFS_DEFAULT_PORT 9000

/* Default data directory */
#define REVFS_DATA_DIR "data"

/* ------- Standard C headers ------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------- POSIX headers (Day 2+) ------- */
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ===================================================================
 *  Day 2 — POSIX File Abstraction
 *
 *  Thin wrappers around raw syscalls.  All functions retry on EINTR
 *  and print diagnostics to stderr on failure.
 * =================================================================== */

/* Open / close */
int      revfs_file_open(const char *path, int flags, mode_t mode);
int      revfs_file_close(int fd);

/* Sequential read / write (move the file offset) */
ssize_t  revfs_file_read(int fd, void *buf, size_t count);
ssize_t  revfs_file_read_all(int fd, void *buf, size_t count);
ssize_t  revfs_file_write(int fd, const void *buf, size_t count);
ssize_t  revfs_file_write_all(int fd, const void *buf, size_t count);

/* Positional read / write (offset-based, no seek) */
ssize_t  revfs_file_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t  revfs_file_pwrite(int fd, const void *buf, size_t count, off_t offset);

/* Seek */
off_t    revfs_file_seek(int fd, off_t offset, int whence);

/* File metadata */
off_t    revfs_file_size(int fd);
off_t    revfs_file_size_path(const char *path);
int      revfs_file_exists(const char *path);

/* Durability */
int      revfs_file_sync(int fd);

/* Convenience */
ssize_t  revfs_file_append(const char *path, const void *buf, size_t count);
int      revfs_mkdir_p(const char *path, mode_t mode);

/* ===================================================================
 *  Day 3 — Chunking + SHA-256 Content-Addressed Storage
 *
 *  Files are split into fixed-size chunks (REVFS_CHUNK_SIZE).
 *  Each chunk is hashed with SHA-256 and stored by its hash.
 *  Duplicate chunks are naturally deduplicated.
 * =================================================================== */

/* SHA-256 produces 32 bytes = 64 hex chars + NUL */
#define REVFS_HASH_HEX_SIZE  65

/* Maximum number of chunks per file (supports files up to ~4 TB) */
#define REVFS_MAX_CHUNKS     1048576

/* SHA-256 hashing */
int      revfs_sha256(const void *data, size_t len, char *hex_out);
int      revfs_sha256_fd(int fd, char *hex_out);

/* Chunk storage (content-addressed) */
int      revfs_chunk_store_path(const char *hash_hex, char *path_out,
                                size_t path_size);
int      revfs_chunk_store(const void *data, size_t len,
                           char *hash_hex_out);
ssize_t  revfs_chunk_load(const char *hash_hex, void *buf,
                           size_t buf_size);
int      revfs_chunk_exists(const char *hash_hex);

/* File chunking / reassembly */
int      revfs_file_chunk(const char *filepath,
                          char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                          int max_chunks);
ssize_t  revfs_chunks_reassemble(const char *output_path,
                                 const char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                                 int num_chunks);

/* ===================================================================
 *  Day 4 — Upload + Metadata Persistence
 *
 *  Files are uploaded by chunking them, storing the chunks, and
 *  writing a metadata file that records the filename, version,
 *  chunk list, size, and timestamp.
 *
 *  Metadata lives at: data/meta/<filename>/v<N>.meta
 *  Format is line-oriented key=value (easy to parse in C).
 * =================================================================== */

/* Maximum filename length (basename only) */
#define REVFS_MAX_FILENAME   256

/* Maximum path length for metadata files */
#define REVFS_MAX_PATH       1024

/* Maximum chunks tracked in a single metadata record.
 * 4096 chunks × 4 MB = 16 GB max file size per version.
 * This keeps revfs_meta_t at ~260 KB (heap-allocatable). */
#define REVFS_META_MAX_CHUNKS  4096

/* Metadata for a single file version */
typedef struct {
    char    name[REVFS_MAX_FILENAME];       /* original basename         */
    int     version;                        /* version number (1, 2, …)  */
    int     num_chunks;                     /* number of chunks          */
    off_t   file_size;                      /* original file size bytes  */
    long    timestamp;                      /* upload time (Unix epoch)  */
    char    chunk_hashes[REVFS_META_MAX_CHUNKS][REVFS_HASH_HEX_SIZE];
} revfs_meta_t;

/* Upload a file: chunk → store → write metadata.
 * Returns the version number on success, -1 on error. */
int      revfs_upload(const char *filepath);

/* Write metadata to disk at data/meta/<name>/v<version>.meta */
int      revfs_meta_write(const revfs_meta_t *meta);

/* Read metadata from disk for a given filename and version.
 * If version == -1, reads the latest version. */
int      revfs_meta_read(const char *filename, int version,
                         revfs_meta_t *meta_out);

/* Determine the next version number for a filename.
 * Returns 1 if no versions exist yet. */
int      revfs_meta_next_version(const char *filename);

/* List all files that have been uploaded.
 * Writes basenames into `names`, returns count or -1 on error. */
int      revfs_meta_list_files(char names[][REVFS_MAX_FILENAME],
                               int max_names);

/* ===================================================================
 *  Day 5 — Download + File Reconstruction
 *
 *  Given a filename and version, reads the metadata, fetches all
 *  chunks from the content-addressed store, and reassembles the
 *  original file at the specified output path.
 * =================================================================== */

/* Download (reconstruct) a file from RevFS.
 * `filename`    — basename of the file (as stored in metadata).
 * `version`     — version to download, or -1 for latest.
 * `output_path` — where to write the reconstructed file.
 * Returns 0 on success, -1 on error. */
int      revfs_download(const char *filename, int version,
                         const char *output_path);

#endif /* REVFS_H */
