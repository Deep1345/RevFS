/*
 * chunk.c — Content-addressed chunk storage with SHA-256
 *
 * Files get split into fixed-size chunks (4 MB default). Each chunk is
 * SHA-256 hashed and stored by its hash, so identical chunks are
 * naturally deduplicated. Uses Apple CommonCrypto on macOS.
 */

#include "revfs.h"
#include <CommonCrypto/CommonDigest.h>

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out)
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2]     = hex_chars[(bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

/* Hash in-memory data → 64-char hex string. */
int revfs_sha256(const void *data, size_t len, char *hex_out)
{
    if (!data || !hex_out) { errno = EINVAL; return -1; }

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)len, digest);
    bytes_to_hex(digest, CC_SHA256_DIGEST_LENGTH, hex_out);
    return 0;
}

/* Hash an open file descriptor (streaming, from current offset). */
int revfs_sha256_fd(int fd, char *hex_out)
{
    if (!hex_out) { errno = EINVAL; return -1; }

    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);

    unsigned char buf[8192];
    ssize_t n;
    while ((n = revfs_file_read(fd, buf, sizeof(buf))) > 0)
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);
    if (n < 0) return -1;

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    bytes_to_hex(digest, CC_SHA256_DIGEST_LENGTH, hex_out);
    return 0;
}

/*
 * Build the storage path for a chunk: data/chunks/<2-char prefix>/<full hash>
 * The prefix subdirectory keeps any single dir from getting too large.
 */
int revfs_chunk_store_path(const char *hash_hex, char *path_out, size_t path_size)
{
    if (!hash_hex || !path_out || path_size == 0) { errno = EINVAL; return -1; }

    int n = snprintf(path_out, path_size, "%s/chunks/%.2s/%s",
                     REVFS_DATA_DIR, hash_hex, hash_hex);
    if (n < 0 || (size_t)n >= path_size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int chunk_dir_path(const char *hash_hex, char *path_out, size_t path_size)
{
    int n = snprintf(path_out, path_size, "%s/chunks/%.2s",
                     REVFS_DATA_DIR, hash_hex);
    if (n < 0 || (size_t)n >= path_size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

/*
 * Store a chunk under its SHA-256 hash. If the chunk already exists
 * (dedup hit), we skip the write entirely and return 1.
 * New chunks are written atomically via temp file + rename.
 */
int revfs_chunk_store(const void *data, size_t len, char *hash_hex_out)
{
    if (!data) { errno = EINVAL; return -1; }

    char hash[REVFS_HASH_HEX_SIZE];
    if (revfs_sha256(data, len, hash) < 0) return -1;
    if (hash_hex_out) memcpy(hash_hex_out, hash, REVFS_HASH_HEX_SIZE);

    char path[512];
    if (revfs_chunk_store_path(hash, path, sizeof(path)) < 0) return -1;

    /* Already exists? Dedup — nothing to do. */
    if (revfs_file_exists(path)) return 1;

    /* Ensure parent dir exists */
    char dir[512];
    if (chunk_dir_path(hash, dir, sizeof(dir)) < 0) return -1;
    if (revfs_mkdir_p(dir, 0755) < 0) return -1;

    /* Write to temp file, then atomic rename (crash-safe, thread-safe) */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld_%p",
             path, (long)getpid(), (void *)pthread_self());

    int fd = revfs_file_open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    if (revfs_file_write_all(fd, data, len) < 0 || revfs_file_sync(fd) < 0) {
        revfs_file_close(fd);
        unlink(tmp_path);
        return -1;
    }
    revfs_file_close(fd);

    if (rename(tmp_path, path) < 0) {
        fprintf(stderr, "revfs: rename(\"%s\" → \"%s\"): %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

/* Load a chunk by hash into caller-provided buffer. Returns bytes read. */
ssize_t revfs_chunk_load(const char *hash_hex, void *buf, size_t buf_size)
{
    if (!hash_hex || !buf) { errno = EINVAL; return -1; }

    char path[512];
    if (revfs_chunk_store_path(hash_hex, path, sizeof(path)) < 0) return -1;

    int fd = revfs_file_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    off_t sz = revfs_file_size(fd);
    if (sz < 0) { revfs_file_close(fd); return -1; }
    if ((size_t)sz > buf_size) {
        fprintf(stderr, "revfs: chunk %s is %lld bytes, buffer is %zu\n",
                hash_hex, (long long)sz, buf_size);
        revfs_file_close(fd);
        errno = ENOBUFS;
        return -1;
    }

    ssize_t r = revfs_file_read_all(fd, buf, (size_t)sz);
    revfs_file_close(fd);
    return r;
}

int revfs_chunk_exists(const char *hash_hex)
{
    if (!hash_hex) return 0;
    char path[512];
    if (revfs_chunk_store_path(hash_hex, path, sizeof(path)) < 0) return 0;
    return revfs_file_exists(path);
}

/*
 * Split a file into fixed-size chunks, hash each one, and store it.
 * Returns the number of chunks created, or -1 on error.
 */
int revfs_file_chunk(const char *filepath,
                     char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                     int max_chunks)
{
    if (!filepath || !chunk_hashes || max_chunks <= 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = revfs_file_open(filepath, O_RDONLY, 0);
    if (fd < 0) return -1;

    off_t file_sz = revfs_file_size(fd);
    if (file_sz < 0) { revfs_file_close(fd); return -1; }

    void *buf = malloc(REVFS_CHUNK_SIZE);
    if (!buf) {
        fprintf(stderr, "revfs: malloc(%d): %s\n", REVFS_CHUNK_SIZE, strerror(errno));
        revfs_file_close(fd);
        return -1;
    }

    int chunk_count = 0;
    off_t remaining = file_sz;

    while (remaining > 0) {
        if (chunk_count >= max_chunks) {
            fprintf(stderr, "revfs: file has more chunks than max_chunks=%d\n", max_chunks);
            free(buf);
            revfs_file_close(fd);
            errno = EOVERFLOW;
            return -1;
        }

        size_t to_read = (remaining > REVFS_CHUNK_SIZE)
                             ? (size_t)REVFS_CHUNK_SIZE
                             : (size_t)remaining;

        ssize_t n = revfs_file_read_all(fd, buf, to_read);
        if (n < 0) { free(buf); revfs_file_close(fd); return -1; }

        int rc = revfs_chunk_store(buf, (size_t)n, chunk_hashes[chunk_count]);
        if (rc < 0) { free(buf); revfs_file_close(fd); return -1; }

        chunk_count++;
        remaining -= n;
    }

    /* Empty files still get a zero-length chunk */
    if (file_sz == 0 && chunk_count == 0) {
        if (max_chunks < 1) { free(buf); revfs_file_close(fd); errno = EOVERFLOW; return -1; }
        if (revfs_chunk_store("", 0, chunk_hashes[0]) < 0) {
            free(buf); revfs_file_close(fd); return -1;
        }
        chunk_count = 1;
    }

    free(buf);
    revfs_file_close(fd);
    return chunk_count;
}

/*
 * Reassemble a file from its ordered list of chunk hashes.
 * Returns total bytes written on success.
 */
ssize_t revfs_chunks_reassemble(const char *output_path,
                                const char chunk_hashes[][REVFS_HASH_HEX_SIZE],
                                int num_chunks)
{
    if (!output_path || !chunk_hashes || num_chunks < 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = revfs_file_open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    void *buf = malloc(REVFS_CHUNK_SIZE);
    if (!buf) {
        fprintf(stderr, "revfs: malloc: %s\n", strerror(errno));
        revfs_file_close(fd);
        return -1;
    }

    ssize_t total = 0;
    for (int i = 0; i < num_chunks; i++) {
        ssize_t chunk_sz = revfs_chunk_load(chunk_hashes[i], buf, (size_t)REVFS_CHUNK_SIZE);
        if (chunk_sz < 0) { free(buf); revfs_file_close(fd); return -1; }

        if (chunk_sz > 0) {
            ssize_t w = revfs_file_write_all(fd, buf, (size_t)chunk_sz);
            if (w < 0) { free(buf); revfs_file_close(fd); return -1; }
            total += w;
        }
    }

    revfs_file_sync(fd);
    revfs_file_close(fd);
    free(buf);
    return total;
}
