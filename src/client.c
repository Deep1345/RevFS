/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 9: Remote Upload/Download over TCP Client
 *
 * This module implements the TCP client connecting to a RevFS server,
 * handling chunk-level transfers, remote metadata queries, distributed
 * deduplication, and high-level remote operations (upload, download,
 * list, history).
 */

#include "revfs.h"
#include <libgen.h>
#include <time.h>

/* Helper: Safe basename extractor */
static int safe_basename(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(path);
    char *tmp = malloc(len + 1);
    if (!tmp) return -1;
    memcpy(tmp, path, len + 1);

    char *base = basename(tmp);
    if (!base || strlen(base) == 0 || strcmp(base, "/") == 0) {
        free(tmp);
        errno = EINVAL;
        return -1;
    }

    if (strlen(base) >= out_size) {
        free(tmp);
        errno = ENAMETOOLONG;
        return -1;
    }

    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
    free(tmp);
    return 0;
}

/* Helper: format human-readable size */
static void format_size(off_t size, char *out, size_t out_len)
{
    if (size < 1024) {
        snprintf(out, out_len, "%lld B", (long long)size);
    } else if (size < 1024 * 1024) {
        snprintf(out, out_len, "%.1f KB", (double)size / 1024.0);
    } else if (size < 1024LL * 1024 * 1024) {
        snprintf(out, out_len, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_len, "%.1f GB",
                 (double)size / (1024.0 * 1024.0 * 1024.0));
    }
}

/* Helper: format timestamp */
static void format_time(long timestamp, char *out, size_t out_len)
{
    time_t t = (time_t)timestamp;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/* Helper: read one line from socket (terminated by \n) */
static int read_socket_line(int sock, char *out_buf, size_t max_size)
{
    if (sock < 0 || !out_buf || max_size == 0) {
        return -1;
    }

    size_t total = 0;
    while (total < max_size - 1) {
        char ch;
        ssize_t n = revfs_file_read(sock, &ch, 1);
        if (n <= 0) {
            return -1; /* Connection closed or error */
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            out_buf[total++] = ch;
        }
    }
    out_buf[total] = '\0';
    return (int)total;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_connect                                              */
/*                                                                    */
/*  Connects to a remote RevFS server at `host`:`port`.               */
/*  Returns connected socket fd on success, -1 on error.              */
/* ------------------------------------------------------------------ */
int revfs_client_connect(const char *host, int port)
{
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "revfs_client: invalid port number: %d\n", port);
        errno = EINVAL;
        return -1;
    }

    const char *target_host = (host && *host) ? host : "127.0.0.1";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "revfs_client: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, target_host, &srv_addr.sin_addr) <= 0) {
        struct hostent *he = gethostbyname(target_host);
        if (!he || !he->h_addr_list[0]) {
            fprintf(stderr, "revfs_client: failed to resolve host '%s'\n", target_host);
            revfs_file_close(sock);
            errno = ENOENT;
            return -1;
        }
        memcpy(&srv_addr.sin_addr, he->h_addr_list[0], sizeof(srv_addr.sin_addr));
    }

    if (connect(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        fprintf(stderr, "revfs_client: connect(%s:%d) failed: %s\n",
                target_host, port, strerror(errno));
        revfs_file_close(sock);
        return -1;
    }

    return sock;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_disconnect                                           */
/*                                                                    */
/*  Gracefully closes client connection by sending QUIT.              */
/* ------------------------------------------------------------------ */
int revfs_client_disconnect(int sock)
{
    if (sock < 0) {
        return -1;
    }
    revfs_file_write_all(sock, "QUIT\n", 5);
    revfs_file_close(sock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_ping                                                 */
/*                                                                    */
/*  Sends PING to remote server and checks response.                  */
/* ------------------------------------------------------------------ */
int revfs_client_ping(int sock, const char *msg, char *resp_out, size_t resp_size)
{
    if (sock < 0) {
        return -1;
    }

    char cmd[512];
    if (msg && *msg) {
        snprintf(cmd, sizeof(cmd), "PING %s\n", msg);
    } else {
        snprintf(cmd, sizeof(cmd), "PING\n");
    }

    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        return -1;
    }

    char resp[512];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (strncmp(resp, "PONG", 4) != 0) {
        return -1;
    }

    if (resp_out && resp_size > 0) {
        strncpy(resp_out, resp, resp_size - 1);
        resp_out[resp_size - 1] = '\0';
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_has_chunk                                            */
/*                                                                    */
/*  Checks if remote server already possesses a chunk with SHA-256    */
/*  hash `hash_hex`. Returns 1 if present, 0 if missing, -1 on error. */
/* ------------------------------------------------------------------ */
int revfs_client_has_chunk(int sock, const char *hash_hex)
{
    if (sock < 0 || !hash_hex || strlen(hash_hex) != 64) {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "HAS_CHUNK %s\n", hash_hex);
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        return -1;
    }

    char resp[128];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (strcmp(resp, "OK 1") == 0) {
        return 1;
    }
    if (strcmp(resp, "OK 0") == 0) {
        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_store_chunk                                          */
/*                                                                    */
/*  Transfers a single chunk payload to the remote server.            */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_client_store_chunk(int sock, const char *hash_hex,
                            const void *data, size_t len)
{
    if (sock < 0 || !hash_hex || strlen(hash_hex) != 64 || len > REVFS_CHUNK_SIZE) {
        return -1;
    }
    if (len > 0 && !data) {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "STORE_CHUNK %s %zu\n", hash_hex, len);
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        return -1;
    }

    if (len > 0) {
        if (revfs_file_write_all(sock, data, len) < 0) {
            return -1;
        }
    }

    char resp[128];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    return (strcmp(resp, "OK") == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_get_chunk                                            */
/*                                                                    */
/*  Downloads a single chunk payload from the remote server.          */
/*  Returns bytes loaded on success, -1 on error.                     */
/* ------------------------------------------------------------------ */
ssize_t revfs_client_get_chunk(int sock, const char *hash_hex,
                              void *buf, size_t buf_size)
{
    if (sock < 0 || !hash_hex || strlen(hash_hex) != 64 || !buf || buf_size == 0) {
        return -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "GET_CHUNK %s\n", hash_hex);
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        return -1;
    }

    char resp[128];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (strncmp(resp, "OK ", 3) != 0) {
        return -1;
    }

    size_t chunk_len = (size_t)atoll(resp + 3);
    if (chunk_len > buf_size) {
        fprintf(stderr, "revfs_client: buffer too small for chunk (%zu > %zu)\n",
                chunk_len, buf_size);
        return -1;
    }

    if (chunk_len > 0) {
        ssize_t r = revfs_file_read_all(sock, buf, chunk_len);
        if (r != (ssize_t)chunk_len) {
            return -1;
        }
    }

    /* Integrity check */
    char calc_hash[REVFS_HASH_HEX_SIZE];
    if (revfs_sha256(buf, chunk_len, calc_hash) < 0 ||
        strcasecmp(calc_hash, hash_hex) != 0) {
        fprintf(stderr, "revfs_client: received corrupted chunk (hash mismatch)\n");
        return -1;
    }

    return (ssize_t)chunk_len;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_get_meta                                             */
/*                                                                    */
/*  Queries remote metadata for a file version (or -1 for latest).    */
/*  Returns 0 on success, -1 on error.                                */
/* ------------------------------------------------------------------ */
int revfs_client_get_meta(int sock, const char *filename, int version,
                          revfs_meta_t *meta_out)
{
    if (sock < 0 || !filename || !meta_out) {
        return -1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "GET_META %s %d\n", filename, version);
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        return -1;
    }

    char resp[512];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (strncmp(resp, "OK ", 3) != 0) {
        return -1;
    }

    memset(meta_out, 0, sizeof(*meta_out));
    strncpy(meta_out->name, filename, sizeof(meta_out->name) - 1);

    long long file_size = 0;
    int num_chunks = 0;
    int ver = 0;
    long timestamp = 0;

    if (sscanf(resp + 3, "%d %lld %d %ld", &ver, &file_size, &num_chunks, &timestamp) != 4) {
        return -1;
    }

    if (num_chunks < 0 || num_chunks > REVFS_META_MAX_CHUNKS) {
        return -1;
    }

    meta_out->version    = ver;
    meta_out->file_size  = (off_t)file_size;
    meta_out->num_chunks = num_chunks;
    meta_out->timestamp  = timestamp;

    for (int i = 0; i < num_chunks; i++) {
        char hash_line[128];
        if (read_socket_line(sock, hash_line, sizeof(hash_line)) < 0) {
            return -1;
        }
        if (strlen(hash_line) != 64) {
            return -1;
        }
        strncpy(meta_out->chunk_hashes[i], hash_line, REVFS_HASH_HEX_SIZE - 1);
    }

    char end_line[64];
    if (read_socket_line(sock, end_line, sizeof(end_line)) < 0 ||
        strcasecmp(end_line, "END") != 0) {
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_upload_meta                                          */
/*                                                                    */
/*  Submits metadata manifest to server to finalize an upload.        */
/*  Returns created version number on success, -1 on error.           */
/* ------------------------------------------------------------------ */
int revfs_client_upload_meta(int sock, const revfs_meta_t *meta)
{
    if (sock < 0 || !meta || meta->num_chunks < 0 || meta->num_chunks > REVFS_META_MAX_CHUNKS) {
        return -1;
    }

    char header[512];
    snprintf(header, sizeof(header), "UPLOAD_META %s %lld %d\n",
             meta->name, (long long)meta->file_size, meta->num_chunks);

    if (revfs_file_write_all(sock, header, strlen(header)) < 0) {
        return -1;
    }

    for (int i = 0; i < meta->num_chunks; i++) {
        char line[128];
        snprintf(line, sizeof(line), "%s\n", meta->chunk_hashes[i]);
        if (revfs_file_write_all(sock, line, strlen(line)) < 0) {
            return -1;
        }
    }

    if (revfs_file_write_all(sock, "END\n", 4) < 0) {
        return -1;
    }

    char resp[128];
    if (read_socket_line(sock, resp, sizeof(resp)) < 0) {
        return -1;
    }

    if (strncmp(resp, "OK ", 3) != 0) {
        return -1;
    }

    return atoi(resp + 3);
}

/* ------------------------------------------------------------------ */
/*  revfs_client_upload                                               */
/*                                                                    */
/*  High-level remote upload pipeline: chunks local file, verifies    */
/*  deduplication with server, stores missing chunks, uploads meta.   */
/* ------------------------------------------------------------------ */
int revfs_client_upload(const char *host, int port, const char *filepath)
{
    if (!filepath || !revfs_file_exists(filepath)) {
        fprintf(stderr, "revfs_client: file not found: '%s'\n",
                filepath ? filepath : "(null)");
        errno = ENOENT;
        return -1;
    }

    off_t file_size = revfs_file_size_path(filepath);
    if (file_size < 0) {
        return -1;
    }

    char filename[REVFS_MAX_FILENAME];
    if (safe_basename(filepath, filename, sizeof(filename)) < 0) {
        fprintf(stderr, "revfs_client: invalid filepath: '%s'\n", filepath);
        return -1;
    }

    int sock = revfs_client_connect(host, port);
    if (sock < 0) {
        return -1;
    }

    /* Open local file for streaming chunks */
    int fd = revfs_file_open(filepath, O_RDONLY, 0);
    if (fd < 0) {
        revfs_client_disconnect(sock);
        return -1;
    }

    int max_chunks_needed = (int)((file_size / REVFS_CHUNK_SIZE) + 2);
    if (max_chunks_needed > REVFS_META_MAX_CHUNKS) {
        max_chunks_needed = REVFS_META_MAX_CHUNKS;
    }
    if (max_chunks_needed < 1) max_chunks_needed = 1;

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        revfs_file_close(fd);
        revfs_client_disconnect(sock);
        return -1;
    }

    strncpy(meta->name, filename, sizeof(meta->name) - 1);
    meta->file_size = file_size;

    void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
    if (!chunk_buf) {
        free(meta);
        revfs_file_close(fd);
        revfs_client_disconnect(sock);
        return -1;
    }

    int num_chunks = 0;
    off_t bytes_read_total = 0;
    int dedup_chunks = 0;

    if (file_size == 0) {
        /* Handle empty file as a single 0-byte chunk */
        char hash_hex[REVFS_HASH_HEX_SIZE];
        if (revfs_sha256("", 0, hash_hex) < 0) goto fail;
        strncpy(meta->chunk_hashes[0], hash_hex, REVFS_HASH_HEX_SIZE - 1);

        int has = revfs_client_has_chunk(sock, hash_hex);
        if (has < 0) goto fail;
        if (!has) {
            if (revfs_client_store_chunk(sock, hash_hex, "", 0) < 0) goto fail;
        } else {
            dedup_chunks++;
        }
        revfs_chunk_store("", 0, NULL);
        num_chunks = 1;
    } else {
        while (bytes_read_total < file_size) {
            size_t to_read = REVFS_CHUNK_SIZE;
            if ((off_t)to_read > file_size - bytes_read_total) {
                to_read = (size_t)(file_size - bytes_read_total);
            }

            ssize_t n = revfs_file_read_all(fd, chunk_buf, to_read);
            if (n != (ssize_t)to_read) {
                goto fail;
            }

            char hash_hex[REVFS_HASH_HEX_SIZE];
            if (revfs_sha256(chunk_buf, (size_t)n, hash_hex) < 0) {
                goto fail;
            }

            if (num_chunks >= REVFS_META_MAX_CHUNKS) {
                fprintf(stderr, "revfs_client: file exceeds maximum supported chunks\n");
                goto fail;
            }

            strncpy(meta->chunk_hashes[num_chunks], hash_hex, REVFS_HASH_HEX_SIZE - 1);

            /* Check remote deduplication */
            int has = revfs_client_has_chunk(sock, hash_hex);
            if (has < 0) {
                goto fail;
            }

            if (!has) {
                if (revfs_client_store_chunk(sock, hash_hex, chunk_buf, (size_t)n) < 0) {
                    goto fail;
                }
            } else {
                dedup_chunks++;
            }

            /* Store in local chunk store for local cache */
            revfs_chunk_store(chunk_buf, (size_t)n, NULL);

            num_chunks++;
            bytes_read_total += n;
        }
    }

    meta->num_chunks = num_chunks;
    revfs_file_close(fd);
    free(chunk_buf);

    int version = revfs_client_upload_meta(sock, meta);
    if (version < 0) {
        fprintf(stderr, "revfs_client: failed to upload metadata\n");
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    printf("Uploaded \"%s\" → version %d (%d chunks, %lld bytes) [remote: %s:%d",
           filename, version, num_chunks, (long long)file_size,
           (host && *host) ? host : "127.0.0.1", port);
    if (dedup_chunks > 0) {
        printf(", %d deduped", dedup_chunks);
    }
    printf("]\n");

    free(meta);
    revfs_client_disconnect(sock);
    return version;

fail:
    free(chunk_buf);
    free(meta);
    revfs_file_close(fd);
    revfs_client_disconnect(sock);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_download                                             */
/*                                                                    */
/*  High-level remote download pipeline: retrieves metadata, pulls    */
/*  chunks from server (or local CAS cache), reassembles to path.     */
/* ------------------------------------------------------------------ */
int revfs_client_download(const char *host, int port, const char *filename,
                          int version, const char *output_path)
{
    if (!filename || !output_path) {
        fprintf(stderr, "revfs_client: invalid arguments for download\n");
        errno = EINVAL;
        return -1;
    }

    int sock = revfs_client_connect(host, port);
    if (sock < 0) {
        return -1;
    }

    revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
    if (!meta) {
        revfs_client_disconnect(sock);
        return -1;
    }

    if (revfs_client_get_meta(sock, filename, version, meta) < 0) {
        fprintf(stderr, "revfs_client: cannot fetch metadata for \"%s\"", filename);
        if (version > 0) fprintf(stderr, " v%d", version);
        fprintf(stderr, "\n");
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    /* Open temp output file */
    char tmp_output[REVFS_MAX_PATH + 16];
    snprintf(tmp_output, sizeof(tmp_output), "%s.tmp", output_path);

    int out_fd = revfs_file_open(tmp_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
    if (!chunk_buf) {
        revfs_file_close(out_fd);
        unlink(tmp_output);
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    off_t total_written = 0;

    for (int i = 0; i < meta->num_chunks; i++) {
        const char *h = meta->chunk_hashes[i];
        ssize_t loaded = -1;

        /* Check if present in local CAS first */
        if (revfs_chunk_exists(h)) {
            loaded = revfs_chunk_load(h, chunk_buf, REVFS_CHUNK_SIZE);
        }

        /* If not found locally, fetch from remote server */
        if (loaded < 0) {
            loaded = revfs_client_get_chunk(sock, h, chunk_buf, REVFS_CHUNK_SIZE);
            if (loaded < 0) {
                fprintf(stderr, "revfs_client: failed to download chunk %d/%d (%s)\n",
                        i + 1, meta->num_chunks, h);
                goto download_fail;
            }
            /* Cache chunk locally */
            revfs_chunk_store(chunk_buf, (size_t)loaded, NULL);
        }

        if (loaded > 0) {
            if (revfs_file_write_all(out_fd, chunk_buf, (size_t)loaded) != loaded) {
                goto download_fail;
            }
            total_written += loaded;
        }
    }

    if (revfs_file_sync(out_fd) < 0) {
        goto download_fail;
    }
    revfs_file_close(out_fd);
    free(chunk_buf);

    if (total_written != meta->file_size) {
        fprintf(stderr, "revfs_client: size mismatch! expected %lld, got %lld\n",
                (long long)meta->file_size, (long long)total_written);
        unlink(tmp_output);
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    if (rename(tmp_output, output_path) < 0) {
        fprintf(stderr, "revfs_client: failed to rename '%s' to '%s': %s\n",
                tmp_output, output_path, strerror(errno));
        unlink(tmp_output);
        free(meta);
        revfs_client_disconnect(sock);
        return -1;
    }

    printf("Downloaded \"%s\" v%d → \"%s\" (%lld bytes, %d chunks) [remote: %s:%d]\n",
           meta->name, meta->version, output_path, (long long)meta->file_size,
           meta->num_chunks, (host && *host) ? host : "127.0.0.1", port);

    free(meta);
    revfs_client_disconnect(sock);
    return 0;

download_fail:
    revfs_file_close(out_fd);
    unlink(tmp_output);
    free(chunk_buf);
    free(meta);
    revfs_client_disconnect(sock);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_list                                                 */
/*                                                                    */
/*  Queries and prints remote repository file catalog.                */
/* ------------------------------------------------------------------ */
int revfs_client_list(const char *host, int port)
{
    int sock = revfs_client_connect(host, port);
    if (sock < 0) {
        return -1;
    }

    if (revfs_file_write_all(sock, "LIST\n", 5) < 0) {
        revfs_client_disconnect(sock);
        return -1;
    }

    char line[512];
    if (read_socket_line(sock, line, sizeof(line)) < 0) {
        revfs_client_disconnect(sock);
        return -1;
    }

    if (strncmp(line, "OK ", 3) != 0) {
        fprintf(stderr, "revfs_client: LIST failed: %s\n", line);
        revfs_client_disconnect(sock);
        return -1;
    }

    int count = atoi(line + 3);

    const char *target = (host && *host) ? host : "127.0.0.1";
    printf("\nFiles stored in Remote RevFS (%s:%d) — %d file%s\n",
           target, port, count, (count == 1) ? "" : "s");
    printf("─────────────────────────────────────────────────────────────────\n");

    while (read_socket_line(sock, line, sizeof(line)) >= 0) {
        if (strcasecmp(line, "END") == 0) {
            break;
        }
        char name[REVFS_MAX_FILENAME];
        int vcount = 0;
        long long sz = 0;
        if (sscanf(line, "%255s %d %lld", name, &vcount, &sz) == 3) {
            char size_str[32];
            format_size((off_t)sz, size_str, sizeof(size_str));
            printf("  %-30s  %2d version%s   latest: %s\n",
                   name, vcount, (vcount == 1) ? " " : "s", size_str);
        }
    }

    printf("─────────────────────────────────────────────────────────────────\n\n");

    revfs_client_disconnect(sock);
    return count;
}

/* ------------------------------------------------------------------ */
/*  revfs_client_history                                              */
/*                                                                    */
/*  Queries and prints version history of a file on remote server.    */
/* ------------------------------------------------------------------ */
int revfs_client_history(const char *host, int port, const char *filename)
{
    if (!filename) {
        return -1;
    }

    int sock = revfs_client_connect(host, port);
    if (sock < 0) {
        return -1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "HISTORY %s\n", filename);
    if (revfs_file_write_all(sock, cmd, strlen(cmd)) < 0) {
        revfs_client_disconnect(sock);
        return -1;
    }

    char line[512];
    if (read_socket_line(sock, line, sizeof(line)) < 0) {
        revfs_client_disconnect(sock);
        return -1;
    }

    if (strncmp(line, "OK ", 3) != 0) {
        fprintf(stderr, "revfs_client: HISTORY failed: %s\n", line);
        revfs_client_disconnect(sock);
        return -1;
    }

    int count = atoi(line + 3);

    const char *target = (host && *host) ? host : "127.0.0.1";
    printf("\nRemote history for \"%s\" (%s:%d) — %d version%s\n",
           filename, target, port, count, (count == 1) ? "" : "s");
    printf("─────────────────────────────────────────────────────────────────\n");

    int idx = 0;
    while (read_socket_line(sock, line, sizeof(line)) >= 0) {
        if (strcasecmp(line, "END") == 0) {
            break;
        }
        int ver = 0, chunks = 0;
        long long sz = 0;
        long ts = 0;
        if (sscanf(line, "v%d %lld %d %ld", &ver, &sz, &chunks, &ts) == 4) {
            idx++;
            char size_str[32];
            char time_str[32];
            format_size((off_t)sz, size_str, sizeof(size_str));
            format_time(ts, time_str, sizeof(time_str));

            printf("  v%-3d  %10s    %2d chunk%-2s   %s%s\n",
                   ver, size_str, chunks, (chunks == 1) ? "" : "s",
                   time_str, (idx == count) ? "  ← latest" : "");
        }
    }

    printf("─────────────────────────────────────────────────────────────────\n\n");

    revfs_client_disconnect(sock);
    return count;
}
