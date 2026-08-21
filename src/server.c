/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Day 8: TCP Server Skeleton
 *
 * This module implements a standalone TCP server built with POSIX sockets.
 * It listens for incoming TCP client connections and implements a
 * line-oriented wire protocol for remote storage operations.
 *
 * Supported commands:
 *   PING [msg]          → PONG [msg]
 *   LIST                → OK <count> files \n <file> <versions> <size> ... \n END
 *   INFO                → OK RevFS <version> ...
 *   HISTORY <file>      → OK <count> versions \n v<N> <size> <chunks> <time> ... \n END
 *   HELP                → OK Supported commands: ...
 *   QUIT / EXIT         → BYE (closes connection)
 */

#include "revfs.h"
#include <signal.h>
#include <ctype.h>
#include <stdarg.h>

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
static int send_all(int fd, const char *data, size_t len)
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

/* ------------------------------------------------------------------ */
/*  revfs_server_process_command                                      */
/*                                                                    */
/*  Parses a single line command and writes the response to client_fd.*/
/*  Returns:                                                          */
/*    1 : success, keep connection alive                             */
/*    0 : success, client requested QUIT / close                      */
/*   -1 : fatal I/O error                                             */
/* ------------------------------------------------------------------ */
int revfs_server_process_command(const char *cmd_line, int client_fd)
{
    if (!cmd_line || client_fd < 0) {
        return -1;
    }

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
        const char *msg = "OK Supported commands: PING, LIST, INFO, HISTORY <file>, HELP, QUIT\n";
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

    /* 6. QUIT / EXIT */
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

    char in_buf[REVFS_MAX_CMD_LEN * 2];
    size_t in_len = 0;

    while (1) {
        ssize_t n = revfs_file_read(client_fd, in_buf + in_len,
                                    sizeof(in_buf) - 1 - in_len);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            /* Client disconnected */
            break;
        }

        in_len += (size_t)n;
        in_buf[in_len] = '\0';

        /* Process complete lines */
        char *line_start = in_buf;
        char *nl;
        while ((nl = strchr(line_start, '\n')) != NULL) {
            *nl = '\0';
            int rc = revfs_server_process_command(line_start, client_fd);
            if (rc <= 0) {
                /* 0 = QUIT, -1 = error: close connection */
                return (rc == 0) ? 0 : -1;
            }
            line_start = nl + 1;
        }

        /* Move remaining partial line to front of in_buf */
        size_t remaining = (size_t)((in_buf + in_len) - line_start);
        if (remaining > 0) {
            memmove(in_buf, line_start, remaining);
        }
        in_len = remaining;
        in_buf[in_len] = '\0';

        /* Avoid buffer overflow from overly long lines without newline */
        if (in_len >= sizeof(in_buf) - 1) {
            send_all(client_fd, "ERR Line too long\n", 18);
            in_len = 0;
            in_buf[0] = '\0';
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
