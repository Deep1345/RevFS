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

#endif /* REVFS_H */
