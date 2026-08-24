/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Entry point and CLI argument parsing.
 */

#include "revfs.h"

static void print_usage(const char *program)
{
    printf("Usage: %s <command> [options]\n", program);
    printf("\n");
    printf("Commands:\n");
    printf("  upload      <file> [options]                      Upload a file (local, remote, or replicated)\n");
    printf("  download    <file> <output> [options]             Download a file (local, remote, or replicated)\n");
    printf("  history     <file> [--host H] [--port P]          Show version history\n");
    printf("  restore     <file> <version>                      Restore a specific version\n");
    printf("  list        [--host H] [--port P]                 List all stored files\n");
    printf("  ping        [msg] [--host H] [--port P]           Ping a RevFS server\n");
    printf("  server      [port] [--threads N]                  Start multi-threaded TCP server (default: 9000, 4 threads)\n");
    printf("  stats       [--host H] [--port P]                 Show storage & deduplication statistics\n");
    printf("  sync        --primary <H:P> --replica <H:P>       Two-way replication sync and auto-repair\n");
    printf("  repl-status --primary <H:P> --replica <H:P>       Check replication cluster health\n");
    printf("\n");
    printf("Options:\n");
    printf("  --host <host>                Remote RevFS server host (default: 127.0.0.1)\n");
    printf("  --port <port>                Remote RevFS server port (default: 9000)\n");
    printf("  --primary <host:port>        Primary node for replication\n");
    printf("  --replica <host:port>        Secondary/Replica node for replication\n");
    printf("  --threads <N>                Number of worker threads (default: 4)\n");
    printf("  --version <N>                Specific file version number\n");
    printf("  --help                       Show this help message\n");
    printf("  --version                    Show version information\n");
}

static void print_version(void)
{
    printf("%s version %s\n", REVFS_NAME, REVFS_VERSION);
}

/* Helper to parse "host:port" or "host" */
static void parse_host_port(const char *str, char *host_out, size_t host_sz, int *port_out)
{
    if (!str || !host_out || host_sz == 0 || !port_out) return;

    char tmp[256];
    strncpy(tmp, str, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *colon = strchr(tmp, ':');
    if (colon) {
        *colon = '\0';
        strncpy(host_out, tmp, host_sz - 1);
        host_out[host_sz - 1] = '\0';
        *port_out = atoi(colon + 1);
        if (*port_out <= 0) *port_out = REVFS_DEFAULT_PORT;
    } else {
        strncpy(host_out, tmp, host_sz - 1);
        host_out[host_sz - 1] = '\0';
        *port_out = REVFS_DEFAULT_PORT;
    }
}

/* Helper to extract --host and --port from CLI argument list */
static void parse_network_flags(int argc, char *argv[], const char **host_out, int *port_out)
{
    *host_out = NULL;
    *port_out = REVFS_DEFAULT_PORT;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            *host_out = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            *port_out = atoi(argv[i + 1]);
            i++;
        }
    }
}

/* Helper to extract replication flags from CLI argument list */
static int parse_replication_flags(int argc, char *argv[], revfs_repl_config_t *cfg_out)
{
    char p_host[128] = "127.0.0.1";
    int p_port = REVFS_DEFAULT_PORT;
    char s_host[128] = "127.0.0.1";
    int s_port = REVFS_DEFAULT_PORT + 1;
    int found = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--primary") == 0 && i + 1 < argc) {
            parse_host_port(argv[i + 1], p_host, sizeof(p_host), &p_port);
            found = 1;
            i++;
        } else if ((strcmp(argv[i], "--replica") == 0 || strcmp(argv[i], "--secondary") == 0) && i + 1 < argc) {
            parse_host_port(argv[i + 1], s_host, sizeof(s_host), &s_port);
            found = 1;
            i++;
        }
    }

    if (found && cfg_out) {
        revfs_repl_config_init(cfg_out, p_host, p_port, s_host, s_port);
    }
    return found;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "upload") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s upload <file> [options]\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filepath = argv[2];

        revfs_repl_config_t cfg;
        if (parse_replication_flags(argc, argv, &cfg)) {
            int version = revfs_repl_upload(&cfg, filepath);
            return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int version = revfs_client_upload(host, port, filepath);
            return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int version = revfs_upload(filepath);
            return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "download") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s download <file> <output> [--version N] [options]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename    = argv[2];
        const char *output_path = argv[3];
        int version = -1;  /* default: latest */

        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
                version = atoi(argv[i + 1]);
                if (version < 1) {
                    fprintf(stderr, "Invalid version: %s\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
                i++;
            }
        }

        revfs_repl_config_t cfg;
        if (parse_replication_flags(argc, argv, &cfg)) {
            int rc = revfs_repl_download(&cfg, filename, version, output_path);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }

        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int rc = revfs_client_download(host, port, filename, version, output_path);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_download(filename, version, output_path);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "history") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s history <file> [--host H] [--port P]\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename = argv[2];
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int rc = revfs_client_history(host, port, filename);
            return (rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_history(filename);
            return (rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "restore") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s restore <file> <version>\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename = argv[2];
        int version = atoi(argv[3]);
        if (version < 1) {
            fprintf(stderr, "Invalid version: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        int new_ver = revfs_restore(filename, version);
        return (new_ver > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "list") == 0) {
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int rc = revfs_client_list(host, port);
            return (rc >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_list_files();
            return (rc >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "ping") == 0) {
        const char *host = "127.0.0.1";
        int port = REVFS_DEFAULT_PORT;
        const char *msg = (argc >= 3 && argv[2][0] != '-') ? argv[2] : NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                host = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = atoi(argv[i + 1]);
                i++;
            }
        }

        int sock = revfs_client_connect(host, port);
        if (sock < 0) {
            return EXIT_FAILURE;
        }
        char resp[256];
        if (revfs_client_ping(sock, msg, resp, sizeof(resp)) < 0) {
            fprintf(stderr, "revfs: ping failed\n");
            revfs_client_disconnect(sock);
            return EXIT_FAILURE;
        }
        printf("%s\n", resp);
        revfs_client_disconnect(sock);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "server") == 0 || strcmp(cmd, "serve") == 0) {
        int port = REVFS_DEFAULT_PORT;
        int threads = REVFS_DEFAULT_THREADS;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
                threads = atoi(argv[i + 1]);
                if (threads <= 0 || threads > REVFS_MAX_THREADS) {
                    fprintf(stderr, "Invalid thread count: %s (must be 1..%d)\n",
                            argv[i + 1], REVFS_MAX_THREADS);
                    return EXIT_FAILURE;
                }
                i++;
            } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = atoi(argv[i + 1]);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
                i++;
            } else if (argv[i][0] != '-') {
                port = atoi(argv[i]);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", argv[i]);
                    return EXIT_FAILURE;
                }
            }
        }
        int rc = revfs_server_start_threaded(port, threads);
        return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "stats") == 0) {
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host || port != REVFS_DEFAULT_PORT) {
            int rc = revfs_client_stats(host ? host : "127.0.0.1", port);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_stats();
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "sync") == 0) {
        revfs_repl_config_t cfg;
        if (!parse_replication_flags(argc, argv, &cfg)) {
            fprintf(stderr, "Usage: %s sync --primary <host:port> --replica <host:port>\n", argv[0]);
            return EXIT_FAILURE;
        }
        int rc = revfs_repl_sync(&cfg, NULL);
        return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "repl-status") == 0) {
        revfs_repl_config_t cfg;
        if (!parse_replication_flags(argc, argv, &cfg)) {
            fprintf(stderr, "Usage: %s repl-status --primary <host:port> --replica <host:port>\n", argv[0]);
            return EXIT_FAILURE;
        }
        int p_ok = 0, s_ok = 0;
        revfs_repl_ping(&cfg, &p_ok, &s_ok);

        printf("\nReplication Cluster Status\n");
        printf("─────────────────────────────────────────────────────────────\n");
        printf("  Primary Node   (%s:%d): %s\n", cfg.primary.host, cfg.primary.port,
               p_ok ? "ONLINE ✅" : "OFFLINE ❌");
        printf("  Secondary Node (%s:%d): %s\n", cfg.secondary.host, cfg.secondary.port,
               s_ok ? "ONLINE ✅" : "OFFLINE ❌");
        printf("─────────────────────────────────────────────────────────────\n\n");
        return (p_ok || s_ok) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
    return EXIT_FAILURE;
}
