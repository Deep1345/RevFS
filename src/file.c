/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 2: POSIX file abstraction layer.
 *
 * Wraps raw POSIX system calls (open, read, write, pread, pwrite,
 * lseek, fsync, close) into a clean API.  This layer intentionally
 * avoids <stdio.h> buffered I/O (fopen / fread / fwrite) so that
 * every I/O operation maps directly to a syscall — giving us full
 * control over caching, sync, and error handling.
 *
 * The chunking engine (Day 3) and storage engine sit on top of this.
 */

#include "revfs.h"

/* ------------------------------------------------------------------ */
/*  revfs_file_open                                                   */
/*                                                                    */
/*  Opens a file and returns its file descriptor.                     */
/*                                                                    */
/*  flags  – standard POSIX flags (O_RDONLY, O_WRONLY | O_CREAT, …)   */
/*  mode   – permission bits when O_CREAT is used (e.g. 0644)        */
/*                                                                    */
/*  Returns the fd on success, or -1 on error (errno is set).        */
/* ------------------------------------------------------------------ */
int revfs_file_open(const char *path, int flags, mode_t mode)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, flags, mode);
    if (fd < 0) {
        fprintf(stderr, "revfs: open(\"%s\"): %s\n", path, strerror(errno));
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_read                                                   */
/*                                                                    */
/*  Reads up to `count` bytes from fd into buf at the current file    */
/*  offset.  Retries on EINTR (signal interruption).                  */
/*                                                                    */
/*  Returns the number of bytes actually read, 0 on EOF, -1 on error.*/
/* ------------------------------------------------------------------ */
ssize_t revfs_file_read(int fd, void *buf, size_t count)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do {
        n = read(fd, buf, count);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        fprintf(stderr, "revfs: read(fd=%d): %s\n", fd, strerror(errno));
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_read_all                                               */
/*                                                                    */
/*  Reads exactly `count` bytes, looping over short reads.            */
/*  Returns count on success, or -1 on error / premature EOF.        */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_read_all(int fd, void *buf, size_t count)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    size_t total = 0;
    while (total < count) {
        ssize_t n = revfs_file_read(fd, (char *)buf + total, count - total);
        if (n < 0)
            return -1;          /* read error */
        if (n == 0) {
            /* EOF before we got all the bytes we wanted */
            errno = EIO;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_write                                                  */
/*                                                                    */
/*  Writes up to `count` bytes from buf to fd at the current offset.  */
/*  Retries on EINTR.                                                 */
/*                                                                    */
/*  Returns number of bytes written, or -1 on error.                 */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_write(int fd, const void *buf, size_t count)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do {
        n = write(fd, buf, count);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        fprintf(stderr, "revfs: write(fd=%d): %s\n", fd, strerror(errno));
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_write_all                                              */
/*                                                                    */
/*  Writes exactly `count` bytes, looping over short writes.          */
/*  Returns count on success, or -1 on error.                        */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_write_all(int fd, const void *buf, size_t count)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    size_t total = 0;
    while (total < count) {
        ssize_t n = revfs_file_write(fd, (const char *)buf + total,
                                     count - total);
        if (n < 0)
            return -1;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_pread                                                  */
/*                                                                    */
/*  Reads up to `count` bytes from fd at a given `offset` WITHOUT     */
/*  modifying the file's current offset.  Retries on EINTR.           */
/*                                                                    */
/*  This is crucial for concurrent reads in the threaded server       */
/*  (Day 10) — multiple threads can read different chunks of the      */
/*  same file descriptor simultaneously.                              */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_pread(int fd, void *buf, size_t count, off_t offset)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do {
        n = pread(fd, buf, count, offset);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        fprintf(stderr, "revfs: pread(fd=%d, off=%lld): %s\n",
                fd, (long long)offset, strerror(errno));
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_pwrite                                                 */
/*                                                                    */
/*  Writes up to `count` bytes to fd at a given `offset` WITHOUT      */
/*  modifying the file's current offset.  Retries on EINTR.           */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n;
    do {
        n = pwrite(fd, buf, count, offset);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        fprintf(stderr, "revfs: pwrite(fd=%d, off=%lld): %s\n",
                fd, (long long)offset, strerror(errno));
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_seek                                                   */
/*                                                                    */
/*  Repositions the file offset.                                      */
/*    whence: SEEK_SET, SEEK_CUR, SEEK_END                           */
/*  Returns the new absolute offset, or -1 on error.                 */
/* ------------------------------------------------------------------ */
off_t revfs_file_seek(int fd, off_t offset, int whence)
{
    off_t pos = lseek(fd, offset, whence);
    if (pos < 0) {
        fprintf(stderr, "revfs: lseek(fd=%d): %s\n", fd, strerror(errno));
    }
    return pos;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_size                                                   */
/*                                                                    */
/*  Returns the size of an open file in bytes using fstat().          */
/*  Returns -1 on error.                                             */
/* ------------------------------------------------------------------ */
off_t revfs_file_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "revfs: fstat(fd=%d): %s\n", fd, strerror(errno));
        return -1;
    }
    return st.st_size;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_size_path                                              */
/*                                                                    */
/*  Returns the size of a file by path (without opening it).         */
/*  Returns -1 on error.                                             */
/* ------------------------------------------------------------------ */
off_t revfs_file_size_path(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "revfs: stat(\"%s\"): %s\n", path, strerror(errno));
        return -1;
    }
    return st.st_size;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_sync                                                   */
/*                                                                    */
/*  Flushes kernel buffers to disk via fsync().  This guarantees      */
/*  that all writes are durable — essential for the WAL journal       */
/*  (Day 13) and metadata persistence.                               */
/*                                                                    */
/*  Returns 0 on success, -1 on error.                               */
/* ------------------------------------------------------------------ */
int revfs_file_sync(int fd)
{
    int rc;
    do {
        rc = fsync(fd);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        fprintf(stderr, "revfs: fsync(fd=%d): %s\n", fd, strerror(errno));
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_close                                                  */
/*                                                                    */
/*  Closes a file descriptor.  Retries on EINTR.                     */
/*  Returns 0 on success, -1 on error.                               */
/* ------------------------------------------------------------------ */
int revfs_file_close(int fd)
{
    int rc;
    do {
        rc = close(fd);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        fprintf(stderr, "revfs: close(fd=%d): %s\n", fd, strerror(errno));
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_exists                                                 */
/*                                                                    */
/*  Checks whether a path exists (file or directory).                */
/*  Returns 1 if it exists, 0 if not.                                */
/* ------------------------------------------------------------------ */
int revfs_file_exists(const char *path)
{
    if (!path)
        return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_mkdir_p                                                     */
/*                                                                    */
/*  Creates a directory (and parent directories) if they don't exist. */
/*  Equivalent to `mkdir -p`.                                        */
/*  Returns 0 on success, -1 on error.                               */
/* ------------------------------------------------------------------ */
int revfs_mkdir_p(const char *path, mode_t mode)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    /* Work on a mutable copy */
    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) {
        fprintf(stderr, "revfs: malloc: %s\n", strerror(errno));
        return -1;
    }
    memcpy(tmp, path, len + 1);

    /* Walk through the path, creating each component */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                fprintf(stderr, "revfs: mkdir(\"%s\"): %s\n",
                        tmp, strerror(errno));
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    /* Create the final directory */
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        fprintf(stderr, "revfs: mkdir(\"%s\"): %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_file_append                                                 */
/*                                                                    */
/*  Opens a file in append mode, writes data, syncs, and closes.     */
/*  Creates the file if it doesn't exist.                            */
/*                                                                    */
/*  Returns count on success, -1 on error.                           */
/* ------------------------------------------------------------------ */
ssize_t revfs_file_append(const char *path, const void *buf, size_t count)
{
    int fd = revfs_file_open(path,
                             O_WRONLY | O_APPEND | O_CREAT,
                             0644);
    if (fd < 0)
        return -1;

    ssize_t written = revfs_file_write_all(fd, buf, count);
    if (written < 0) {
        revfs_file_close(fd);
        return -1;
    }

    if (revfs_file_sync(fd) < 0) {
        revfs_file_close(fd);
        return -1;
    }

    revfs_file_close(fd);
    return written;
}
