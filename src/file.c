/*
 * file.c — POSIX file abstraction layer
 *
 * Thin wrappers around raw syscalls. We avoid stdio buffered I/O
 * on purpose so we get full control over caching and fsync behavior.
 */

#include "revfs.h"

int revfs_file_open(const char *path, int flags, mode_t mode)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, flags, mode);
    if (fd < 0)
        fprintf(stderr, "revfs: open(\"%s\"): %s\n", path, strerror(errno));
    return fd;
}

int revfs_file_close(int fd)
{
    int rc;
    do { rc = close(fd); } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        fprintf(stderr, "revfs: close(fd=%d): %s\n", fd, strerror(errno));
    return rc;
}

/* Single read — retries on EINTR, returns bytes read or -1. */
ssize_t revfs_file_read(int fd, void *buf, size_t count)
{
    if (!buf) { errno = EINVAL; return -1; }

    ssize_t n;
    do { n = read(fd, buf, count); } while (n < 0 && errno == EINTR);
    if (n < 0)
        fprintf(stderr, "revfs: read(fd=%d): %s\n", fd, strerror(errno));
    return n;
}

/* Loops until exactly `count` bytes are read (or EOF/error). */
ssize_t revfs_file_read_all(int fd, void *buf, size_t count)
{
    if (!buf) { errno = EINVAL; return -1; }

    size_t total = 0;
    while (total < count) {
        ssize_t n = revfs_file_read(fd, (char *)buf + total, count - total);
        if (n < 0) return -1;
        if (n == 0) { errno = EIO; return -1; }  /* unexpected EOF */
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* Single write — retries on EINTR. */
ssize_t revfs_file_write(int fd, const void *buf, size_t count)
{
    if (!buf) { errno = EINVAL; return -1; }

    ssize_t n;
    do { n = write(fd, buf, count); } while (n < 0 && errno == EINTR);
    if (n < 0)
        fprintf(stderr, "revfs: write(fd=%d): %s\n", fd, strerror(errno));
    return n;
}

/* Loops until exactly `count` bytes are written. */
ssize_t revfs_file_write_all(int fd, const void *buf, size_t count)
{
    if (!buf) { errno = EINVAL; return -1; }

    size_t total = 0;
    while (total < count) {
        ssize_t n = revfs_file_write(fd, (const char *)buf + total,
                                     count - total);
        if (n < 0) return -1;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* Positional read — doesn't move the file offset, safe for concurrent use. */
ssize_t revfs_file_pread(int fd, void *buf, size_t count, off_t offset)
{
    if (!buf) { errno = EINVAL; return -1; }

    ssize_t n;
    do { n = pread(fd, buf, count, offset); } while (n < 0 && errno == EINTR);
    if (n < 0)
        fprintf(stderr, "revfs: pread(fd=%d, off=%lld): %s\n",
                fd, (long long)offset, strerror(errno));
    return n;
}

/* Positional write — doesn't move the file offset. */
ssize_t revfs_file_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (!buf) { errno = EINVAL; return -1; }

    ssize_t n;
    do { n = pwrite(fd, buf, count, offset); } while (n < 0 && errno == EINTR);
    if (n < 0)
        fprintf(stderr, "revfs: pwrite(fd=%d, off=%lld): %s\n",
                fd, (long long)offset, strerror(errno));
    return n;
}

off_t revfs_file_seek(int fd, off_t offset, int whence)
{
    off_t pos = lseek(fd, offset, whence);
    if (pos < 0)
        fprintf(stderr, "revfs: lseek(fd=%d): %s\n", fd, strerror(errno));
    return pos;
}

off_t revfs_file_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "revfs: fstat(fd=%d): %s\n", fd, strerror(errno));
        return -1;
    }
    return st.st_size;
}

off_t revfs_file_size_path(const char *path)
{
    if (!path) { errno = EINVAL; return -1; }

    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "revfs: stat(\"%s\"): %s\n", path, strerror(errno));
        return -1;
    }
    return st.st_size;
}

int revfs_file_exists(const char *path)
{
    if (!path) return 0;
    struct stat st;
    return (stat(path, &st) == 0);
}

/* Flush kernel buffers to disk — critical for WAL and metadata durability. */
int revfs_file_sync(int fd)
{
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        fprintf(stderr, "revfs: fsync(fd=%d): %s\n", fd, strerror(errno));
    return rc;
}

/* Recursive mkdir, like `mkdir -p`. */
int revfs_mkdir_p(const char *path, mode_t mode)
{
    if (!path) { errno = EINVAL; return -1; }

    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) {
        fprintf(stderr, "revfs: malloc: %s\n", strerror(errno));
        return -1;
    }
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
                fprintf(stderr, "revfs: mkdir(\"%s\"): %s\n", tmp, strerror(errno));
                free(tmp);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
        fprintf(stderr, "revfs: mkdir(\"%s\"): %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

/* Open, append, sync, close — convenience wrapper. */
ssize_t revfs_file_append(const char *path, const void *buf, size_t count)
{
    int fd = revfs_file_open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return -1;

    ssize_t written = revfs_file_write_all(fd, buf, count);
    if (written < 0) { revfs_file_close(fd); return -1; }
    if (revfs_file_sync(fd) < 0) { revfs_file_close(fd); return -1; }

    revfs_file_close(fd);
    return written;
}
