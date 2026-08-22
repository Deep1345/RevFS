/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 8 + Day 9: TCP Server & Wire Protocol
 *
 * This module implements a standalone TCP server built with POSIX sockets.
 * It listens for incoming TCP client connections and implements a
 * line-oriented wire protocol for remote storage operations.
 *
 * Supported commands:
 *   PING [msg]                         → PONG [msg]
 *   LIST                               → OK <count> files \n <file> <versions> <size> ... \n END
 *   INFO                               → OK RevFS <version> ...
 *   HISTORY <file>                     → OK <count> versions \n v<N> <size> <chunks> <time> ... \n END
 *   HAS_CHUNK <hash>                   → OK 1 (exists) | OK 0 (missing)
 *   STORE_CHUNK <hash> <len> \n <data> → OK | ERR <reason>
 *   GET_CHUNK <hash>                   → OK <len> \n <raw_bytes> | ERR <reason>
 *   GET_META <file> [version]          → OK <ver> <size> <chunks> <time> \n <hash0> ... \n END
 *   UPLOAD_META <file> <size> <count>  → (reads <count> hashes + END) → OK <version>
 *   HELP                               → OK Supported commands: ...
 *   QUIT / EXIT                        → BYE (closes connection)
 */

#include "revfs.h"
#include <signal.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* Stream context for buffered socket I/O */
typedef struct {
    int    fd;
    char   in_buf[REVFS_MAX_CMD_LEN * 4];
    size_t in_len;
} stream_t;

/* Server running flag (allows clean shutdown) */
static volatile sig_atomic_t g_server_running = 0;

/* Signal handler for graceful termination */
static void handle_signal(int sig)
{
    (void)sig;
    g_server_running = 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_server_stop                                                 */
/*                                                                    */
/*  Signals the server accept loop to stop running.                   */
/* ------------------------------------------------------------------ */
void revfs_server_stop(void)
{
    g_server_running = 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_server_create                                               */
/*                                                                    */
/*  Creates, configures, binds, and listens on a TCP socket.          */
/*  If `actual_port` is non-NULL, populates it with the bound port.   */
/*  Returns the listening socket fd, or -1 on error.                 */
/* ------------------------------------------------------------------ */
int revfs_server_create(int port, int *actual_port)
{
    if (port < 0 || port > 65535) {
        fprintf(stderr, "revfs_server: invalid port number: %d\n", port);
        errno = EINVAL;
        return -1;
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "revfs_server: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "revfs_server: setsockopt(SO_REUSEADDR) failed: %s\n",
                strerror(errno));
        revfs_file_close(listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "revfs_server: bind(port=%d) failed: %s\n",
                port, strerror(errno));
        revfs_file_close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, REVFS_SERVER_BACKLOG) < 0) {
        fprintf(stderr, "revfs_server: listen() failed: %s\n", strerror(errno));
        revfs_file_close(listen_fd);
        return -1;
    }

    if (actual_port) {
        struct sockaddr_in bound_addr;
        socklen_t len = sizeof(bound_addr);
        if (getsockname(listen_fd, (struct sockaddr *)&bound_addr, &len) == 0) {
            *actual_port = ntohs(bound_addr.sin_port);
        } else {
            *actual_port = port;
        }
    }

    return listen_fd;
}

/* Helper to send all bytes to a socket descriptor */
static int send_all(int fd, const void *data, size_t len)
{
    ssize_t written = revfs_file_write_all(fd, data, len);
    return (written == (ssize_t)len) ? 0 : -1;
}

/* Helper to send formatted string response */
static int send_formatted(int fd, const char *fmt, ...)
{
    char buf[REVFS_MAX_RESP_LEN];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    return send_all(fd, buf, (size_t)n);
}

/* Helper to read exact count bytes from buffered stream */
static ssize_t stream_read_bytes(stream_t *s, void *dest, size_t count)
{
    size_t done = 0;
    if (s->in_len > 0) {
        size_t take = (s->in_len < count) ? s->in_len : count;
        memcpy(dest, s->in_buf, take);
        done += take;
        size_t rem = s->in_len - take;
        if (rem > 0) {
            memmove(s->in_buf, s->in_buf + take, rem);
        }
        s->in_len = rem;
        s->in_buf[s->in_len] = '\0';
    }
    if (done < count) {
        ssize_t r = revfs_file_read_all(s->fd, (char *)dest + done, count - done);
        if (r < 0) return -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* Helper to read one line (terminated by \n) from buffered stream */
static int stream_read_line(stream_t *s, char *line_out, size_t max_size)
{
    while (1) {
        char *nl = memchr(s->in_buf, '\n', s->in_len);
        if (nl) {
            size_t line_len = (size_t)(nl - s->in_buf);
            size_t copy_len = (line_len < max_size - 1) ? line_len : max_size - 1;
            memcpy(line_out, s->in_buf, copy_len);
            line_out[copy_len] = '\0';
            if (copy_len > 0 && line_out[copy_len - 1] == '\r') {
                line_out[copy_len - 1] = '\0';
            }
            size_t rem = s->in_len - (line_len + 1);
            if (rem > 0) {
                memmove(s->in_buf, nl + 1, rem);
            }
            s->in_len = rem;
            s->in_buf[s->in_len] = '\0';
            return 0;
        }
        if (s->in_len >= sizeof(s->in_buf) - 1) {
            return -2; /* Line too long */
        }
        ssize_t n = revfs_file_read(s->fd, s->in_buf + s->in_len,
                                    sizeof(s->in_buf) - 1 - s->in_len);
        if (n <= 0) {
            return -1; /* EOF or disconnect */
        }
        s->in_len += (size_t)n;
        s->in_buf[s->in_len] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/*  revfs_server_process_command_stream                               */
/* ------------------------------------------------------------------ */
static int revfs_server_process_command_stream(const char *cmd_line, stream_t *stream)
{
    if (!cmd_line || !stream || stream->fd < 0) {
        return -1;
    }

    int client_fd = stream->fd;

    /* Trim leading whitespace */
    while (*cmd_line && isspace((unsigned char)*cmd_line)) {
        cmd_line++;
    }

    /* Empty line - ignore and keep open */
    if (*cmd_line == '\0') {
        return 1;
    }

    /* Copy to local buffer for tokenization */
    char buf[REVFS_MAX_CMD_LEN];
    snprintf(buf, sizeof(buf), "%s", cmd_line);

    /* Trim trailing whitespace and newlines */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || isspace((unsigned char)buf[len - 1]))) {
        buf[len - 1] = '\0';
        len--;
    }

    if (len == 0) {
        return 1;
    }

    /* Parse command name and argument */
    char *arg = strchr(buf, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg && isspace((unsigned char)*arg)) {
            arg++;
        }
    }

    /* 1. PING [msg] */
    if (strcasecmp(buf, "PING") == 0) {
        if (arg && *arg) {
            if (send_formatted(client_fd, "PONG %s\n", arg) < 0) return -1;
        } else {
            if (send_all(client_fd, "PONG\n", 5) < 0) return -1;
        }
        return 1;
    }

    /* 2. INFO */
    if (strcasecmp(buf, "INFO") == 0) {
        if (send_formatted(client_fd, "OK RevFS %s (chunk_size=%d, default_port=%d)\n",
                           REVFS_VERSION, REVFS_CHUNK_SIZE, REVFS_DEFAULT_PORT) < 0) {
            return -1;
        }
        return 1;
    }

    /* 3. HELP */
    if (strcasecmp(buf, "HELP") == 0) {
        const char *msg = "OK Supported commands: PING, LIST, INFO, HISTORY <file>, HAS_CHUNK <hash>, STORE_CHUNK <hash> <len>, GET_CHUNK <hash>, GET_META <file> [ver], UPLOAD_META <file> <size> <count>, HELP, QUIT\n";
        if (send_all(client_fd, msg, strlen(msg)) < 0) return -1;
        return 1;
    }

    /* 4. LIST */
    if (strcasecmp(buf, "LIST") == 0) {
        char names[128][REVFS_MAX_FILENAME];
        int count = revfs_meta_list_files(names, 128);
        if (count < 0) {
            if (send_all(client_fd, "ERR Failed to list files\n", 25) < 0) return -1;
            return 1;
        }

        if (send_formatted(client_fd, "OK %d files\n", count) < 0) return -1;

        revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
        for (int i = 0; i < count; i++) {
            int vcount = revfs_version_count(names[i]);
            off_t sz = 0;
            if (meta && revfs_meta_read(names[i], -1, meta) == 0) {
                sz = meta->file_size;
            }
            if (send_formatted(client_fd, "%s %d %lld\n",
                               names[i], vcount, (long long)sz) < 0) {
                free(meta);
                return -1;
            }
        }
        free(meta);

        if (send_all(client_fd, "END\n", 4) < 0) return -1;
        return 1;
    }

    /* 5. HISTORY <file> */
    if (strcasecmp(buf, "HISTORY") == 0) {
        if (!arg || *arg == '\0') {
            if (send_all(client_fd, "ERR Missing filename for HISTORY\n", 33) < 0) return -1;
            return 1;
        }

        int count = revfs_version_count(arg);
        if (count <= 0) {
            if (send_all(client_fd, "ERR No versions found for file\n", 31) < 0) return -1;
            return 1;
        }

        if (send_formatted(client_fd, "OK %d versions\n", count) < 0) return -1;

        revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
        if (!meta) {
            if (send_all(client_fd, "ERR Memory allocation error\n", 28) < 0) return -1;
            return 1;
        }

        for (int v = 1; v <= count; v++) {
            if (revfs_meta_read(arg, v, meta) == 0) {
                if (send_formatted(client_fd, "v%d %lld %d %ld\n",
                                   meta->version, (long long)meta->file_size,
                                   meta->num_chunks, meta->timestamp) < 0) {
                    free(meta);
                    return -1;
                }
            }
        }
        free(meta);

        if (send_all(client_fd, "END\n", 4) < 0) return -1;
        return 1;
    }

    /* 6. HAS_CHUNK <hash> */
    if (strcasecmp(buf, "HAS_CHUNK") == 0) {
        if (!arg || strlen(arg) != 64) {
            if (send_all(client_fd, "ERR Invalid hash\n", 17) < 0) return -1;
            return 1;
        }
        int exists = revfs_chunk_exists(arg);
        if (send_formatted(client_fd, "OK %d\n", exists ? 1 : 0) < 0) return -1;
        return 1;
    }

    /* 7. STORE_CHUNK <hash> <len> */
    if (strcasecmp(buf, "STORE_CHUNK") == 0) {
        if (!arg) {
            if (send_all(client_fd, "ERR Missing arguments for STORE_CHUNK\n", 38) < 0) return -1;
            return 1;
        }
        char hash_hex[REVFS_HASH_HEX_SIZE];
        size_t chunk_len = 0;
        if (sscanf(arg, "%64s %zu", hash_hex, &chunk_len) != 2) {
            if (send_all(client_fd, "ERR Invalid STORE_CHUNK format\n", 31) < 0) return -1;
            return 1;
        }

        if (chunk_len > REVFS_CHUNK_SIZE) {
            if (send_all(client_fd, "ERR Chunk exceeds maximum size\n", 31) < 0) return -1;
            return 1;
        }

        void *chunk_data = malloc(chunk_len > 0 ? chunk_len : 1);
        if (!chunk_data) {
            if (send_all(client_fd, "ERR Memory allocation failed\n", 29) < 0) return -1;
            return 1;
        }

        if (chunk_len > 0) {
            ssize_t read_bytes = stream_read_bytes(stream, chunk_data, chunk_len);
            if (read_bytes != (ssize_t)chunk_len) {
                free(chunk_data);
                return -1; /* Connection error */
            }
        }

        /* Verify SHA-256 checksum */
        char calc_hash[REVFS_HASH_HEX_SIZE];
        if (revfs_sha256(chunk_data, chunk_len, calc_hash) < 0 ||
            strcasecmp(calc_hash, hash_hex) != 0) {
            free(chunk_data);
            if (send_all(client_fd, "ERR Hash mismatch\n", 18) < 0) return -1;
            return 1;
        }

        /* Store chunk into CAS */
        int store_rc = revfs_chunk_store(chunk_data, chunk_len, NULL);
        free(chunk_data);

        if (store_rc < 0) {
            if (send_all(client_fd, "ERR Failed to store chunk\n", 26) < 0) return -1;
            return 1;
        }

        if (send_all(client_fd, "OK\n", 3) < 0) return -1;
        return 1;
    }

    /* 8. GET_CHUNK <hash> */
    if (strcasecmp(buf, "GET_CHUNK") == 0) {
        if (!arg || strlen(arg) != 64) {
            if (send_all(client_fd, "ERR Invalid hash\n", 17) < 0) return -1;
            return 1;
        }

        if (!revfs_chunk_exists(arg)) {
            if (send_all(client_fd, "ERR Chunk not found\n", 20) < 0) return -1;
            return 1;
        }

        void *chunk_buf = malloc(REVFS_CHUNK_SIZE);
        if (!chunk_buf) {
            if (send_all(client_fd, "ERR Memory error\n", 17) < 0) return -1;
            return 1;
        }

        ssize_t loaded = revfs_chunk_load(arg, chunk_buf, REVFS_CHUNK_SIZE);
        if (loaded < 0) {
            free(chunk_buf);
            if (send_all(client_fd, "ERR Failed to read chunk\n", 25) < 0) return -1;
            return 1;
        }

        if (send_formatted(client_fd, "OK %zd\n", loaded) < 0) {
            free(chunk_buf);
            return -1;
        }

        if (loaded > 0) {
            if (send_all(client_fd, chunk_buf, (size_t)loaded) < 0) {
                free(chunk_buf);
                return -1;
            }
        }

        free(chunk_buf);
        return 1;
    }

    /* 9. GET_META <file> [version] */
    if (strcasecmp(buf, "GET_META") == 0) {
        if (!arg || *arg == '\0') {
            if (send_all(client_fd, "ERR Missing filename for GET_META\n", 34) < 0) return -1;
            return 1;
        }

        char filename[REVFS_MAX_FILENAME];
        int version = -1;
        if (sscanf(arg, "%255s %d", filename, &version) < 1) {
            if (send_all(client_fd, "ERR Invalid GET_META arguments\n", 31) < 0) return -1;
            return 1;
        }

        revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
        if (!meta) {
            if (send_all(client_fd, "ERR Memory error\n", 17) < 0) return -1;
            return 1;
        }

        if (revfs_meta_read(filename, version, meta) < 0) {
            free(meta);
            if (send_all(client_fd, "ERR File or version not found\n", 30) < 0) return -1;
            return 1;
        }

        if (send_formatted(client_fd, "OK %d %lld %d %ld\n",
                           meta->version, (long long)meta->file_size,
                           meta->num_chunks, meta->timestamp) < 0) {
            free(meta);
            return -1;
        }

        for (int i = 0; i < meta->num_chunks; i++) {
            if (send_formatted(client_fd, "%s\n", meta->chunk_hashes[i]) < 0) {
                free(meta);
                return -1;
            }
        }
        free(meta);

        if (send_all(client_fd, "END\n", 4) < 0) return -1;
        return 1;
    }

    /* 10. UPLOAD_META <file> <size> <num_chunks> */
    if (strcasecmp(buf, "UPLOAD_META") == 0) {
        if (!arg) {
            if (send_all(client_fd, "ERR Missing arguments for UPLOAD_META\n", 38) < 0) return -1;
            return 1;
        }

        char filename[REVFS_MAX_FILENAME];
        long long file_size = 0;
        int num_chunks = 0;
        if (sscanf(arg, "%255s %lld %d", filename, &file_size, &num_chunks) != 3) {
            if (send_all(client_fd, "ERR Invalid UPLOAD_META format\n", 31) < 0) return -1;
            return 1;
        }

        if (num_chunks < 0 || num_chunks > REVFS_META_MAX_CHUNKS) {
            if (send_all(client_fd, "ERR Invalid chunk count\n", 24) < 0) return -1;
            return 1;
        }

        revfs_meta_t *meta = calloc(1, sizeof(revfs_meta_t));
        if (!meta) {
            if (send_all(client_fd, "ERR Memory error\n", 17) < 0) return -1;
            return 1;
        }

        strncpy(meta->name, filename, sizeof(meta->name) - 1);
        meta->file_size  = (off_t)file_size;
        meta->num_chunks = num_chunks;
        meta->timestamp  = (long)time(NULL);

        int read_error = 0;
        char line_buf[REVFS_MAX_CMD_LEN];
        for (int i = 0; i < num_chunks; i++) {
            if (stream_read_line(stream, line_buf, sizeof(line_buf)) < 0) {
                read_error = 1;
                break;
            }
            if (strlen(line_buf) != 64) {
                read_error = 2;
                break;
            }
            strncpy(meta->chunk_hashes[i], line_buf, REVFS_HASH_HEX_SIZE - 1);
        }

        /* Read trailing END marker */
        if (!read_error) {
            if (stream_read_line(stream, line_buf, sizeof(line_buf)) < 0 ||
                strcasecmp(line_buf, "END") != 0) {
                read_error = 3;
            }
        }

        if (read_error) {
            free(meta);
            if (send_all(client_fd, "ERR Failed reading chunk list\n", 30) < 0) return -1;
            return 1;
        }

        /* Verify all chunks exist in local CAS */
        for (int i = 0; i < num_chunks; i++) {
            if (!revfs_chunk_exists(meta->chunk_hashes[i])) {
                free(meta);
                if (send_all(client_fd, "ERR Incomplete upload: missing chunks\n", 38) < 0) return -1;
                return 1;
            }
        }

        int next_ver = revfs_meta_next_version(filename);
        if (next_ver < 0) {
            free(meta);
            if (send_all(client_fd, "ERR Failed determining next version\n", 36) < 0) return -1;
            return 1;
        }
        meta->version = next_ver;

        if (revfs_meta_write(meta) < 0) {
            free(meta);
            if (send_all(client_fd, "ERR Failed to persist metadata\n", 31) < 0) return -1;
            return 1;
        }

        free(meta);
        if (send_formatted(client_fd, "OK %d\n", next_ver) < 0) return -1;
        return 1;
    }

    /* 11. QUIT / EXIT */
    if (strcasecmp(buf, "QUIT") == 0 || strcasecmp(buf, "EXIT") == 0) {
        send_all(client_fd, "BYE\n", 4);
        return 0;  /* Signal client handler to terminate connection */
    }

    /* Unknown command */
    if (send_formatted(client_fd, "ERR Unknown command: %s\n", buf) < 0) {
        return -1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  revfs_server_process_command                                      */
/*                                                                    */
/*  Public API function for backward-compatibility.                   */
/* ------------------------------------------------------------------ */
int revfs_server_process_command(const char *cmd_line, int client_fd)
{
    stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.fd = client_fd;
    return revfs_server_process_command_stream(cmd_line, &stream);
}

/* ------------------------------------------------------------------ */
/*  revfs_server_handle_client                                        */
/*                                                                    */
/*  Reads commands line-by-line from `client_fd` until connection is  */
/*  closed by client or QUIT is received.                             */
/* ------------------------------------------------------------------ */
int revfs_server_handle_client(int client_fd)
{
    if (client_fd < 0) {
        return -1;
    }

    stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.fd = client_fd;

    char line[REVFS_MAX_CMD_LEN];
    while (1) {
        int r = stream_read_line(&stream, line, sizeof(line));
        if (r < 0) {
            if (r == -2) {
                send_all(client_fd, "ERR Line too long\n", 18);
                continue;
            }
            /* EOF or disconnect */
            break;
        }

        int rc = revfs_server_process_command_stream(line, &stream);
        if (rc <= 0) {
            return (rc == 0) ? 0 : -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  revfs_server_start                                                */
/*                                                                    */
/*  Main server entry point: sets up socket, listens, and accepts     */
/*  client connections in a loop.                                     */
/* ------------------------------------------------------------------ */
int revfs_server_start(int port)
{
    /* Ignore SIGPIPE so write to closed client doesn't crash server */
    signal(SIGPIPE, SIG_IGN);

    /* Hook SIGINT / SIGTERM for graceful shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int actual_port = 0;
    int listen_fd = revfs_server_create(port, &actual_port);
    if (listen_fd < 0) {
        return -1;
    }

    g_server_running = 1;
    printf("\n╔══════════════════════════════════════════════╗\n");
    printf("║         RevFS Server %-10s              ║\n", REVFS_VERSION);
    printf("║  Listening on port: %-5d                    ║\n", actual_port);
    printf("║  Data directory:    %-20s     ║\n", REVFS_DATA_DIR);
    printf("║  Press Ctrl+C to stop                        ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    struct pollfd pfd;
    pfd.fd     = listen_fd;
    pfd.events = POLLIN;

    while (g_server_running) {
        /* Poll with 500ms timeout to allow responsive shutdown */
        int prc = poll(&pfd, 1, 500);
        if (prc < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "revfs_server: poll error: %s\n", strerror(errno));
            break;
        }
        if (prc == 0) {
            /* Timeout: continue loop */
            continue;
        }

        if (pfd.revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) continue;
                fprintf(stderr, "revfs_server: accept error: %s\n", strerror(errno));
                continue;
            }

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            printf("revfs_server: client connected from %s:%d (fd=%d)\n",
                   client_ip, ntohs(client_addr.sin_port), client_fd);

            revfs_server_handle_client(client_fd);

            printf("revfs_server: client disconnected (fd=%d)\n", client_fd);
            revfs_file_close(client_fd);
        }
    }

    printf("\nrevfs_server: shutting down...\n");
    revfs_file_close(listen_fd);
    return 0;
}
